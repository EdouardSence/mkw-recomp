#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include "cfg.h"
#include "vk_exec.h"
#include "vk_mem_internal.h"

// ---------------------------------------------------------------- MKW_SHADOW_LOST (diagnosis, default 0)
// What the shadow (vk_mem_shadow.c) loses of the CPU's writes, per presented frame, against the console, where the CPU
// and the GPU share the bytes. Found for the flickering trees (06/11/2026): their indirect arguments are written by the
// CPU every frame and completed by a culling dispatch that binds them writable, and a sync skips every byte of a range
// some recorded GPU write may touch.
// - skip: CPU-written bytes a sync leaves out (guest != shadow), split by whether every pending write over them belongs
//   to the submission being synced ("cur": the GPU has not run yet, so the GPU reads stale bytes the console would
//   not) or some belongs to an earlier one ("old": the shadow may hold GPU bytes not copied back).
// - clobber (2): at the write-back, bytes the GPU left as they were and the CPU changed: the copy replaces the CPU's
//   value with the stale one. Needs a copy of the shadow at record time of each written range up to LOST_BASE_MAX;
//   ranges merged by bytes (MKW_SHADOW_MERGE) revert nothing and are not counted.
// - args: indirect draws whose arguments in the shadow differ from guest memory right after the sync of their
//   submission, per VS. Not a loss by itself: a culling dispatch of an earlier part may have written them already, and
//   guest memory gets its bytes at the fence (lf2: 317 frames with the merge, no skip left).
// Guest pages under a read trap are not read (it would fill them): counted apart.
int lost_on = -1;
#define LOST_BASE_MAX (64u << 10)
#define LOST_TOP 6

typedef struct { uint64_t addr, size, bytes; int kind; } LostRun;
typedef struct { uint64_t addr, size, frame; const uint8_t *shadow; uint32_t vs; } LostWatch;
typedef struct { uint32_t vs, n, bad; } LostVs;

static PlatMutex lost_lock = PLAT_MUTEX_INIT;   // innermost
static uint64_t l_cur, l_old, l_clob, l_conf, l_trap, l_nobase, l_runs;
static LostRun top[LOST_TOP];
static LostWatch *watch;
static int nwatch, cwatch;
static LostVs vs_acc[64];
static int nvs;

int lost_level(void) {
    if (lost_on < 0) {
        const char *e = cfg_get("MKW_SHADOW_LOST");
        lost_on = e ? atoi(e) : 0;
        if (lost_on) rt_log("mem: lost CPU writes diagnosed, level %d (MKW_SHADOW_LOST)", lost_on);
    }
    return lost_on;
}

static void top_note(uint64_t addr, uint64_t size, uint64_t bytes, int kind) {   // lost_lock held
    int k = -1;
    for (int i = 0; i < LOST_TOP; i++)
        if (k < 0 || top[i].bytes < top[k].bytes) k = i;
    if (bytes > top[k].bytes) top[k] = (LostRun){addr, size, bytes, kind};
}

// Bytes of [a, b) that differ between guest memory and p (b - a bytes), leaving out trapped pages (*trapped).
static uint64_t diff_guest(uint64_t a, uint64_t b, const uint8_t *p, uint64_t *trapped) {
    uint64_t n = 0;
    for (uint64_t x = a, y; x < b; x = y) {
        y = (x & ~0xfffull) + 0x1000;
        if (y > b) y = b;
        if (mem_trapped(x)) { *trapped += y - x; continue; }
        const uint8_t *g = (const uint8_t *)(uintptr_t)x, *s = p + (x - a);
        if (!memcmp(g, s, y - x)) continue;
        for (uint64_t i = 0; i < y - x; i++) n += g[i] != s[i];
    }
    return n;
}

// shadow_lock held: a sync skips [lo, hi) of r; old: some pending write over it is from an earlier submission.
void lost_skip(const PoolRec *r, uint64_t lo, uint64_t hi, int old) {
    uint64_t t = 0, n = diff_guest(lo, hi, r->smap + (lo - r->import_lo), &t);
    plat_mutex_lock(&lost_lock);
    l_trap += t;
    if (n) {
        l_runs++;
        *(old ? &l_old : &l_cur) += n;
        top_note(lo, hi - lo, n, old ? 'o' : 'c');
    }
    plat_mutex_unlock(&lost_lock);
}

// At record time (vk_mem_gpu_pending, no lock held): the shadow's bytes of a range the GPU may write, the base the
// write-back compares with. NULL above LOST_BASE_MAX or outside a shadow.
uint8_t *lost_base(uint64_t addr, uint64_t size) {
    if (lost_level() < 2) return NULL;
    uint8_t *b = NULL;
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, addr, size);
    const PoolRec *r = e ? e->buf : NULL;
    if (size <= LOST_BASE_MAX && r && r->smap && addr >= r->import_lo && addr + size <= r->import_hi &&
        (b = malloc(size)))
        memcpy(b, r->smap + (addr - r->import_lo), size);
    plat_mutex_unlock_shared(&pool_lock);
    if (!b) {
        plat_mutex_lock(&lost_lock);
        l_nobase += size;
        plat_mutex_unlock(&lost_lock);
    }
    return b;
}

// Write-back of [addr, addr+size) from shadow s, before it: base is the shadow at record time (freed here).
void lost_writeback(uint64_t addr, uint64_t size, const uint8_t *s, uint8_t *base) {
    uint64_t clob = 0, conf = 0, t = 0;
    for (uint64_t x = addr, y; x < addr + size; x = y) {
        y = (x & ~0xfffull) + 0x1000;
        if (y > addr + size) y = addr + size;
        if (mem_trapped(x)) { t += y - x; continue; }
        const uint8_t *g = (const uint8_t *)(uintptr_t)x;
        uint64_t o = x - addr;
        if (!memcmp(g, base + o, y - x)) continue;   // the CPU changed nothing here
        for (uint64_t i = 0; i < y - x; i++)
            if (g[i] != base[o + i]) {
                if (s[o + i] == base[o + i]) clob++;
                else if (s[o + i] != g[i]) conf++;
            }
    }
    free(base);
    plat_mutex_lock(&lost_lock);
    l_clob += clob, l_conf += conf, l_trap += t;
    if (clob) top_note(addr, size, clob, 'w');
    plat_mutex_unlock(&lost_lock);
}

// Replay: an indirect draw of `vs` reads its arguments at [addr, addr+size) of shadow pool r.
void vk_mem_lost_watch(const void *pool, uint64_t addr, uint64_t size, uint32_t vs) {
    const PoolRec *r = pool;
    if (lost_level() < 1 || !r || !r->smap || addr < r->import_lo || addr + size > r->import_hi) return;
    plat_mutex_lock(&lost_lock);
    if (nwatch == cwatch) vk_mem_grow((void **)&watch, &cwatch, sizeof *watch);
    watch[nwatch++] = (LostWatch){addr, size, vk_exec_presented, r->smap + (addr - r->import_lo), vs};
    plat_mutex_unlock(&lost_lock);
}

// Right after a sync, shadow_lock held: the watched draws of the synced submission.
void lost_check(void) {
    plat_mutex_lock(&lost_lock);
    for (int i = 0; i < nwatch; i++) {
        const LostWatch *w = &watch[i];
        uint64_t t = 0;
        int bad = diff_guest(w->addr, w->addr + w->size, w->shadow, &t) != 0;
        int k = 0;
        while (k < nvs && vs_acc[k].vs != w->vs) k++;
        if (k == nvs) {
            if (nvs == (int)(sizeof vs_acc / sizeof *vs_acc)) continue;
            vs_acc[nvs++] = (LostVs){w->vs, 0, 0};
        }
        vs_acc[k].n++, vs_acc[k].bad += (uint32_t)bad;
    }
    nwatch = 0;
    plat_mutex_unlock(&lost_lock);
}

// At each present: one line when something was lost since the previous one.
void vk_mem_lost_log(uint64_t frame) {
    if (lost_on <= 0) return;
    plat_mutex_lock(&lost_lock);
    char vs[512] = "", runs[512] = "";
    int o = 0, any_bad = 0;
    for (int i = 0; i < nvs; i++)
        if (vs_acc[i].bad) {
            any_bad = 1;
            o += snprintf(vs + o, sizeof vs - (size_t)o, " %08x:%u/%u", vs_acc[i].vs, vs_acc[i].bad, vs_acc[i].n);
            if (o >= (int)sizeof vs) break;
        }
    o = 0;
    for (int i = 0; i < LOST_TOP; i++)
        if (top[i].bytes && o < (int)sizeof runs)
            o += snprintf(runs + o, sizeof runs - (size_t)o, " %c%#llx+%#llx:%llu", top[i].kind,
                          (unsigned long long)top[i].addr, (unsigned long long)top[i].size,
                          (unsigned long long)top[i].bytes);
    if (l_cur || l_old || l_clob || l_conf || any_bad)
        rt_log("lost: f%llu skip cur %llu old %llu B in %llu runs, clobber %llu conflict %llu B, trapped %llu, "
               "no base %llu; args differ (vs:bad/draws)%s; runs%s",
               (unsigned long long)frame, (unsigned long long)l_cur, (unsigned long long)l_old,
               (unsigned long long)l_runs, (unsigned long long)l_clob, (unsigned long long)l_conf,
               (unsigned long long)l_trap, (unsigned long long)l_nobase, vs, runs);
    l_cur = l_old = l_clob = l_conf = l_trap = l_nobase = l_runs = 0;
    memset(top, 0, sizeof top);
    nvs = 0;
    plat_mutex_unlock(&lost_lock);
}
