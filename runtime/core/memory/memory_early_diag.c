// MKW_EARLY_READ_DRY's events (diagnosis), for memory_early.c: split out of it, which had grown past 350 lines.
// MKW_EARLY_READ_DRY=1 ("ab"; diagnosis): =3's waits are logged, not made, with the game's call chain; "diff": made,
// and each logs how many bytes of the page it changed and the read bytes without and with it. And with the
// watch on, the pattern behind them: the GPU's bytes reach guest memory from the live shadow (a read trap's fill,
// memory.c; a merged write-back, vk_mem_wback.c), so a submission the GPU is running then that writes the same bytes
// can hand the game its half-done state (one dispatch clears counters another adds to: tnone1, the clouds gone for a
// frame). Logged each frame: every such fill or write-back, the page, the writer running and the bytes the game got.
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "kernel_int.h"
#include "memory_int.h"

#define PAGE 0x1000ull
#define EV_MAX 400
static PlatMutex ev_lock = PLAT_MUTEX_INIT;
static struct { int kind; uint64_t frame, addr, w, done, pc, lr[4]; uint8_t b[16]; } ev[64];
static int nev, ev_logged[3];   // logged so far by kind, EV_MAX / 3 each: the write-backs filled it alone in tdry1
static int64_t ev_cnt[3];   // since the last log line: dry waits, fills from a running writer, write-backs likewise
static const char *const ev_name[3] = {"wait skipped", "fill while its writer runs", "write-back while its writer runs"};

// ev_lock taken here: one event; pc and the guest chain only for a game thread's access.
void mem_early_ev_note(int kind, uint64_t addr, uint64_t w, uint64_t done, uint64_t pc) {
    plat_mutex_lock(&ev_lock);
    ev_cnt[kind]++;
    if (nev < 64 && ev_logged[kind] < EV_MAX / 3) {
        ev_logged[kind]++;
        __typeof__(ev[0]) *e = &ev[nev++];
        memset(e, 0, sizeof *e);
        e->kind = kind, e->frame = mem_early_frame, e->addr = addr, e->w = w, e->done = done, e->pc = pc;
        plat_mem_peek(e->b, addr & ~15ull, 16);
        const Cpu *c = tcur && pc ? tcur->cur : NULL;
        if (c) {   // the frame chain on the guest stack, read through any protection (a fault here would deadlock)
            e->lr[0] = c->x[30];
            uint64_t fp = c->x[29], fr[2];
            for (int i = 1; i < 4 && fp && !(fp & 7) && plat_mem_peek(fr, fp, 16); i++) e->lr[i] = fr[1], fp = fr[0] > fp ? fr[0] : 0;
        }
    }
    plat_mutex_unlock(&ev_lock);
}

// The writer of [a, a+n)'s pages the GPU may be running now: the value after max(done, after), when it has a part on
// the queue and is the page's last or previous listed writer. 0 none (or no watch).
uint64_t mem_early_inflight(uint64_t a, uint64_t n, uint64_t after) {
    if (!n || mem_early_on() <= 0 || !mem_fence_done || !mem_fence_queued || a + n > RT_ADDRESS_LIMIT) return 0;
    uint64_t done = mem_fence_done(), run = (done > after ? done : after) + 1;
    if (run > mem_fence_queued()) return 0;
    for (uint64_t p = a & ~(PAGE - 1); p < a + n; p += PAGE)
        if (mem_lastf[p / PAGE] == run || mem_prevf[p / PAGE] == run) return run;
    return 0;
}

static int census(void) {   // only with MKW_EARLY_READ_DRY set: a diagnosis stays off the default path
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("MKW_EARLY_READ_DRY");
        on = e && *e && strcmp(e, "0");
    }
    return on;
}

// frz_lock held: the trapped page of a was just filled from the live shadow (memory.c); access: by the fault at a.
void mem_early_filled_locked(uint64_t a, int access) {
    if (!census()) return;
    uint64_t w = mem_early_inflight(a, 1, 0);
    if (w) mem_early_ev_note(1, a, w, mem_fence_done(), access && tcur ? plat_fault_pc : 0);
}

// The merged write-back of fence f (vk_mem_wback.c), copied from the live shadow.
void mem_early_wb_check(uint64_t a, uint64_t n, uint64_t f) {
    if (!census()) return;
    uint64_t w = mem_early_inflight(a, n, f);
    if (w) mem_early_ev_note(2, a, w, mem_fence_done ? mem_fence_done() : 0, 0);
}

void mem_early_ev_log(void) {   // every frame: the events since the last call
    if (!__atomic_load_n(&nev, __ATOMIC_RELAXED)) return;
    plat_mutex_lock(&ev_lock);
    for (int i = 0; i < nev; i++) {
            char d[4][96];
            for (int k = 0; k < 4; k++) ldr_describe(ev[i].lr[k], d[k], sizeof d[k]);
            const uint8_t *b = ev[i].b;
            rt_log("mem: early %s at frame %llu: %#llx, writer %llu (GPU at %llu), by exe+%#llx, guest %s < %s < %s < %s; "
                   "bytes %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                   ev_name[ev[i].kind], (unsigned long long)ev[i].frame, (unsigned long long)ev[i].addr,
                   (unsigned long long)ev[i].w, (unsigned long long)ev[i].done,
                   (unsigned long long)(ev[i].pc ? ev[i].pc - plat_exe_base() : 0), d[0], d[1], d[2], d[3], b[0], b[1],
                   b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        }
    nev = 0;
    plat_mutex_unlock(&ev_lock);
}

// The counts for the line every 600 frames, then reset.
int mem_early_ev_counts(char *b, int n) {
    int k = snprintf(b, (size_t)n, "%s %lld, %s %lld, %s %lld", ev_name[0], (long long)ev_cnt[0], ev_name[1],
                     (long long)ev_cnt[1], ev_name[2], (long long)ev_cnt[2]);
    memset(ev_cnt, 0, sizeof ev_cnt);
    return k;
}
