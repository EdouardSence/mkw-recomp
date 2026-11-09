// The game clock the shared runtime reads (host.h frame_step_*), for Mario Kart World: counted from presents, and
// for the A/B blocks from the phase MKW_SEQ_UNTIL waits for. totk-pc-recomp's resolution.c and frame_rate.c (patch
// sites and fixed addresses of TotK) have no MKW counterpart.
//
// Frame rate (MKW_FPS, Screen tab): the game picks its present interval each frame in main+0x3a9e28 (a virtual of its
// frame object, column main+0x329fc70 slot 28) from the time it measured for the frame, and its game clock
// ([main+0x33e6ae8]) advances by that interval: a frame at interval 1 is 1/60 s of game time. Our pacer waits where
// the game does not count it, so on a machine near 30 fps the interval flips between 1 and 2 and the race runs slow
// (Adrià 09/11/2026: "como si estuviera en modo 60 yendo a 30"). [obj+0x210] = 1 or 2 is the game's own fixed
// interval, which skips that choice; 30 and 60 write it before each call, auto leaves the game's value.
#include <stdatomic.h>
#include <string.h>

#include "cfg.h"
#include "host.h"

#define GUEST_FRAME_UPDATE 0x70003a9e28ull        // MKW 1.0.0 main: the frame object's per-frame virtual
#define GUEST_FRAME_UPDATE_SLOT 0x700329fd50ull   // its vtable slot (main's data segment, writable)
#define GUEST_GAME_CLOCK 0x70033e6ae8ull          // u32, += the present interval every frame
#define FRAME_FIXED 0x210                         // frame object: fixed interval, 0 = the game picks
#define FRAME_HOST 0xfa0                          // patch page offset of host_frame_update (patches.c)
#define SPEED_WINDOW_NS 5000000000ull

static _Atomic uint64_t ticks;   // 60 Hz ticks since the first present

// MKW has no save-load point: the A/B blocks (MKW_AB, perf.c rt_ab) count from the phase MKW_SEQ_UNTIL waits for
// (the race's start in tools/run/race.sh), so both halves measure the same scene.
int64_t frame_step_world_us(void) { return seqwatch_since_us(); }

uint64_t frame_step_ticks(void) { return atomic_load_explicit(&ticks, memory_order_relaxed); }

// Game speed, for tools/run/summary.py: game seconds per real second over each window, and how often the interval
// changed. Below 1 the game runs in slow motion.
static struct { uint64_t t0; uint32_t clock0; unsigned frames, changes; int interval; } sp;

static void speed_note(int interval) {
    uint64_t now = kern_now_ns();
    uint32_t clock = *(const volatile uint32_t *)(uintptr_t)GUEST_GAME_CLOCK;
    if (!sp.t0) {
        sp.t0 = now, sp.clock0 = clock, sp.interval = interval;
        return;
    }
    sp.frames++;
    if (interval != sp.interval) sp.changes++, sp.interval = interval;
    if (now - sp.t0 < SPEED_WINDOW_NS) return;
    double real = (double)(now - sp.t0) / 1e9;
    rt_log("game: speed %.2fx over %.1f s (%u frames, %u interval changes, MKW_FPS %s)",
           (double)(clock - sp.clock0) / 60.0 / real, real, sp.frames, sp.changes, cfg_value("MKW_FPS"));
    sp.t0 = now, sp.clock0 = clock, sp.frames = sp.changes = 0;
}

void frame_step_present(uint64_t display_ns, int interval) {
    (void)display_ns;
    atomic_fetch_add_explicit(&ticks, interval > 0 ? (uint64_t)interval : 1, memory_order_relaxed);
    speed_note(interval);
}

// The game thread, once per frame: MKW_FPS into the fixed interval, then the game's own function. saved: the game's
// value before the first forced frame, put back when the setting returns to auto.
static void host_frame_update(Cpu *c) {
    static int saved = -1;
    uint32_t *fixed = (uint32_t *)(uintptr_t)(c->x[0] + FRAME_FIXED);
    const char *fps = cfg_value("MKW_FPS");
    int want = !strcmp(fps, "30") ? 2 : !strcmp(fps, "60") ? 1 : 0;
    if (want) {
        if (saved < 0) {
            saved = (int)*fixed;
            rt_log("game: present interval fixed at %d (MKW_FPS %s; the game's value was %d)", want, fps, saved);
        }
        *fixed = (uint32_t)want;
    } else if (saved >= 0) {
        *fixed = (uint32_t)saved;
        saved = -1;
        rt_log("game: present interval chosen by the game again (MKW_FPS auto)");
    }
    rt_call(c, GUEST_FRAME_UPDATE, 1, c->x[0]);
}

void frame_rate_host(uint64_t patch_page) { rt_register_host(patch_page + FRAME_HOST, host_frame_update); }

// After the loader relocated main: the vtable slot calls host_frame_update instead.
void frame_rate_init(uint64_t patch_page) {
    uint64_t *slot = (uint64_t *)(uintptr_t)GUEST_FRAME_UPDATE_SLOT;
    if (*slot != GUEST_FRAME_UPDATE) {
        rt_log("game: vtable slot %#llx holds %#llx, not the frame update: MKW_FPS has no effect",
               (unsigned long long)GUEST_FRAME_UPDATE_SLOT, (unsigned long long)*slot);
        return;
    }
    *slot = patch_page + FRAME_HOST;
}
