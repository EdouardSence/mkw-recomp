// Turbo: the game runs `speed` times faster, to get through menus, movies and loads sooner.
// MKW_TURBO = off (default) | hold (while F2 is held) | toggle (F2 turns it on and off) | on (the whole run, for the
// scripts) | until (on until MKW_SEQ_UNTIL's event, for the run scripts: what they measure goes at normal speed);
// MKW_TURBO_SPEED = 2..8 (default 4). Both are read live, once per present.
//
// Three clocks pace the game and all three go `speed` times faster: the present pacer (nvn_api_obj.c present_period),
// the 60 Hz vsync thread (gfx_present.c vsync_main) and the guest clock the game reads with nn::os::GetSystemTick
// (main.c rt_ticks). The first two make the parts that count frames go faster, the third the parts that measure time;
// with one left out the game would run some things fast and others not (its 30 or 60 fps choice reads the clock).
// When the machine cannot reach `speed` times the console's frame rate the game behaves as on a slow PC at normal
// speed: frames late against the clock. The window shows at most 60 frames a second (turbo_drop_present): the
// skipped ones are still drawn, as the game reads back what the GPU wrote.
#include <stdatomic.h>
#include <string.h>

#include "cfg.h"
#include "cpu.h"
#include "hacks.h"
#include "host.h"
#include "input.h"

#define SHOW_NS (1000000000ull / 60 - 1000000ull)   // 60 Hz with 1 ms of slack, so a 60 Hz present is not dropped

static _Atomic int speed = 1;
static int toggled, key_was;

// The guest clock: real ticks since r0 times mul, from v0. A seqlock: writers (turbo_poll, one thread) bump seq to odd,
// change the fields and bump it to even; a reader retries while seq is odd or changed.
static struct { _Atomic uint32_t seq; _Atomic uint64_t r0, v0, mul; } clk = {0, 0, 0, 1};

uint64_t turbo_clock(uint64_t real) {
    for (;;) {
        uint32_t s = atomic_load_explicit(&clk.seq, memory_order_acquire);
        uint64_t r0 = atomic_load_explicit(&clk.r0, memory_order_relaxed);
        uint64_t v0 = atomic_load_explicit(&clk.v0, memory_order_relaxed);
        uint64_t mul = atomic_load_explicit(&clk.mul, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (!(s & 1) && atomic_load_explicit(&clk.seq, memory_order_relaxed) == s)
            return real >= r0 ? v0 + (real - r0) * mul : v0;   // a tick read before the change: no step back
    }
}

// A new factor from now on: the guest clock keeps its value and only its rate changes, so it never steps back.
static void clock_rate(int mul) {
    uint64_t real = rt_ticks_real(), v = turbo_clock(real);
    atomic_fetch_add_explicit(&clk.seq, 1, memory_order_acq_rel);
    atomic_store_explicit(&clk.r0, real, memory_order_relaxed);
    atomic_store_explicit(&clk.v0, v, memory_order_relaxed);
    atomic_store_explicit(&clk.mul, (uint64_t)mul, memory_order_relaxed);
    atomic_fetch_add_explicit(&clk.seq, 1, memory_order_release);
}

int turbo_speed(void) { return atomic_load_explicit(&speed, memory_order_relaxed); }

void turbo_poll(void) {
    const char *mode = cfg_value("MKW_TURBO");
    int key = 0;
    if (!strcmp(mode, "hold") || !strcmp(mode, "toggle")) {
        int down = input_hack_key(HACK_KEY_TURBO);
        if (down && !key_was) toggled = !toggled;
        key_was = down;
        key = mode[0] == 'h' ? down : toggled;
    }
    int on = !strcmp(mode, "on") || key || (!strcmp(mode, "until") && seqwatch_since_us() < 0);
    int want = on ? cfg_int("MKW_TURBO_SPEED") : 1;
    if (want < 1) want = 1;
    if (want > 8) want = 8;
    int had = atomic_load_explicit(&speed, memory_order_relaxed);
    if (want == had) return;
    clock_rate(want);
    atomic_store_explicit(&speed, want, memory_order_relaxed);
    rt_log("turbo: x%d (MKW_TURBO=%s)", want, mode);
}

int turbo_drop_present(uint64_t now) {
    static _Atomic uint64_t shown;
    uint64_t last = atomic_load_explicit(&shown, memory_order_relaxed);
    if (turbo_speed() > 1 && now - last < SHOW_NS) return 1;
    atomic_store_explicit(&shown, now, memory_order_relaxed);
    return 0;
}
