#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "platform/platform.h"
#include "vk_exec.h"
#include "vk_mem_internal.h"

// ---------------------------------------------------------------- MKW_SHADOW_RACE (diagnosis, default 0)
// The GPU reads a shadow pool (vk_mem_shadow.c) straight from its host-visible buffer, and the sync before the next
// submission copies the CPU's newer bytes into that same buffer: if the GPU is still running an earlier submission that
// reads them, that one sees the newer bytes. A capture's serial replay drew a black cactus in one frame of some replays
// and not in others, with every buffer of the draws the same at the pass end. Here every shadow
// range a recording resolves is noted with its submission's fence; a sync that copies bytes different from the shadow's
// over a noted range whose fence is not done yet and is not the one being synced logs it ("srace:" lines, the first
// RACE_LINES, then a count per frame line with the shadow line).

#define RACE_LINES 20000

typedef struct { const PoolRec *r; uint64_t lo, hi, fence; } RNote;

int race_on = -1;
static PlatMutex race_lock = PLAT_MUTEX_INIT;
static RNote *notes;   // race_lock
static int nnotes, cnotes;
static uint64_t r_hits, r_bytes, r_lines;   // race_lock: since the last log, and lines written
static uint8_t *tmp;
static uint64_t ctmp;

// MKW_FENCE_EARLY with its precise guard (MKW_FENCE_EARLY_GUARD unset or 2; vk_sync.c prologue): the same notes, and a
// copy that would change bytes a running submission reads first waits for that submission's GPU work (race_on 2: no
// srace lines). The game, told a fence is done once recorded, rewrote such bytes when the GPU ran behind (fe3: 20
// copies at the first frame of a B block, the GPU 6 submissions behind); a wait before every part cost 7.7 ms each (fe1).
static uint64_t r_waits, r_wait_ns;   // shadow_lock: since the last log

int race_level(void) {
    if (race_on < 0) {
        const char *e = cfg_get("MKW_SHADOW_RACE"), *fe = cfg_get("MKW_FENCE_EARLY"), *g = cfg_get("MKW_FENCE_EARLY_GUARD");
        race_on = e && *e && strcmp(e, "0") ? 1 : fe && *fe && strcmp(fe, "0") && (!g || atoi(g) == 2) ? 2 : 0;
        if (race_on == 1) rt_log("mem: shadow copies over ranges a running submission reads are logged (MKW_SHADOW_RACE)");
        if (race_on == 2) rt_log("mem: shadow copies over ranges a running submission reads wait for it while fences end early (MKW_FENCE_EARLY)");
    }
    return race_on;
}

// shadow_lock held: race_check found the copy changes bytes that fence, not done, reads; with fences ending early, the
// copy waits for its GPU work (the timeline itself: done also waits for write-backs, which may need shadow_lock).
void race_wait(uint64_t fence) {
    if (!fence || !mem_fence_early) return;
    uint64_t t0 = kern_now_ns();
    VkCtx *c = vk_ctx();
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                              .pSemaphores = &c->timeline, .pValues = &fence};
    vkWaitSemaphores(c->device, &wi, 2000000000ull);
    r_waits++, r_wait_ns += kern_now_ns() - t0;
}

// Recording thread, at a resolve of shadow pool r.
void race_note(const PoolRec *r, uint64_t lo, uint64_t hi) {
    uint64_t f = vk_exec_recording_fence();
    if (!f || hi <= lo) return;
    plat_mutex_lock(&race_lock);
    for (int i = nnotes - 1; i >= 0 && i >= nnotes - 8; i--)
        if (notes[i].fence == f && notes[i].lo <= lo && notes[i].hi >= hi) {
            plat_mutex_unlock(&race_lock);
            return;
        }
    if (nnotes == cnotes) vk_mem_grow((void **)&notes, &cnotes, sizeof *notes);
    notes[nnotes++] = (RNote){r, lo, hi, f};
    plat_mutex_unlock(&race_lock);
}

// shadow_lock held, before [a, b) of r is copied in from guest memory for the submission with fence sync; or, old not
// NULL, after a merge by bytes changed it (vk_mem_merge.c): old holds the shadow's bytes from before.
uint64_t race_check(const PoolRec *r, uint64_t a, uint64_t b, uint64_t sync, const uint8_t *old) {
    uint64_t done = vk_sync_fence_done(), wait = 0;
    plat_mutex_lock(&race_lock);
    int k = 0;
    for (int i = 0; i < nnotes; i++)
        if (notes[i].fence > done) notes[k++] = notes[i];   // the finished ones go
    nnotes = k;
    for (int i = 0; i < nnotes; i++) {
        const RNote *n = &notes[i];
        if (n->r != r || n->fence >= sync || n->lo >= b || n->hi <= a) continue;
        uint64_t lo = n->lo > a ? n->lo : a, hi = n->hi < b ? n->hi : b, len = hi - lo;
        if (len > ctmp && !(tmp = realloc(tmp, ctmp = len))) rt_fatal("vk: out of memory (MKW_SHADOW_RACE)");
        if (old) memcpy(tmp, old + (lo - a), len);
        else mem_copy_out(tmp, lo, len);
        const uint8_t *s = r->smap + (lo - r->import_lo);
        uint64_t diff = 0, first = UINT64_MAX, last = 0;
        for (uint64_t x = 0; x < len; x++)
            if (tmp[x] != s[x]) diff++, last = x, first = first == UINT64_MAX ? x : first;
        if (!diff) continue;
        r_hits++, r_bytes += diff;
        if (n->fence > wait) wait = n->fence;
        if (race_on == 1 && r_lines++ < RACE_LINES)
            rt_log("srace: f%llu %s sync %llu (done %llu) over [%#llx, %#llx) read by %llu: %llu bytes differ in [%#llx, %#llx]",
                   (unsigned long long)vk_exec_presented, old ? "merge" : "copy", (unsigned long long)sync, (unsigned long long)done,
                   (unsigned long long)n->lo, (unsigned long long)n->hi, (unsigned long long)n->fence,
                   (unsigned long long)diff, (unsigned long long)(lo + first), (unsigned long long)(lo + last));
    }
    plat_mutex_unlock(&race_lock);
    return wait;
}

void race_log(uint64_t frame) {
    plat_mutex_lock(&race_lock);
    rt_log("mem: shadow race at frame %llu: %llu copies over ranges a running submission reads, %llu bytes changed; "
           "%d ranges noted; %llu copies waited for it, %.1f ms", (unsigned long long)frame, (unsigned long long)r_hits,
           (unsigned long long)r_bytes, nnotes, (unsigned long long)r_waits, r_wait_ns / 1e6);
    r_hits = r_bytes = r_waits = r_wait_ns = 0;
    plat_mutex_unlock(&race_lock);
}

// ---------------------------------------------------------------- MKW_SHADOW_WATCH=<lo>-<hi>[,...] (hex; diagnosis)
// Every change the shadow syncs make to the shadow inside [lo, hi), every pending GPU write they skip there and every
// write-back over it, one "swatch:" line each with the frame and the submission: the order of CPU copies and GPU
// results over one buffer (the per-instance records the culling dispatch writes).
uint64_t watch_hi;   // the highest end: 0 without the switch
static uint64_t wr[4][2];
static int nwr;

void watch_init(void) {
    const char *e = cfg_get("MKW_SHADOW_WATCH");
    for (const char *p = e; p && nwr < 4 && strchr(p, '-');) {
        char *end;
        wr[nwr][0] = strtoull(p, &end, 16);
        wr[nwr][1] = strtoull(end + 1, &end, 16);
        if (wr[nwr][1] > watch_hi) watch_hi = wr[nwr][1];
        rt_log("mem: shadow changes over [%#llx, %#llx) logged (MKW_SHADOW_WATCH)", (unsigned long long)wr[nwr][0],
               (unsigned long long)wr[nwr][1]);
        nwr++;
        p = *end == ',' ? end + 1 : NULL;
    }
}

static int watched_range(uint64_t a, uint64_t b) {
    for (int i = 0; i < nwr; i++)
        if (a < wr[i][1] && b > wr[i][0]) return 1;
    return 0;
}

static void watch_one(const PoolRec *r, uint64_t lo, uint64_t hi, uint64_t sync);

// shadow_lock held, before [a, b) of r is copied in from guest memory for the submission with fence sync.
void watch_copy(const PoolRec *r, uint64_t a, uint64_t b, uint64_t sync) {
    for (int i = 0; i < nwr; i++) {
        uint64_t lo = a > wr[i][0] ? a : wr[i][0], hi = b < wr[i][1] ? b : wr[i][1];
        if (lo < hi) watch_one(r, lo, hi, sync);
    }
}

static void watch_one(const PoolRec *r, uint64_t lo, uint64_t hi, uint64_t sync) {
    uint64_t len = hi - lo;
    plat_mutex_lock(&race_lock);
    if (len > ctmp && !(tmp = realloc(tmp, ctmp = len))) rt_fatal("vk: out of memory (MKW_SHADOW_WATCH)");
    mem_copy_out(tmp, lo, len);
    const uint8_t *s = r->smap + (lo - r->import_lo);
    uint64_t diff = 0, zero = 0, first = 0;
    for (uint64_t x = 0; x < len; x++)
        if (tmp[x] != s[x]) first = diff++ ? first : x, zero += !tmp[x];
    if (diff)
        rt_log("swatch: f%llu sync %llu (done %llu) copy [%#llx, %#llx): %llu bytes change from %#llx, %llu of them to zero",
               (unsigned long long)vk_exec_presented, (unsigned long long)sync,
               (unsigned long long)vk_sync_fence_done(), (unsigned long long)lo, (unsigned long long)hi,
               (unsigned long long)diff, (unsigned long long)(lo + first), (unsigned long long)zero);
    plat_mutex_unlock(&race_lock);
}

void watch_event(const char *what, uint64_t lo, uint64_t hi, uint64_t fence, uint64_t bytes) {
    if (!watched_range(lo, hi)) return;
    rt_log("swatch: f%llu %s [%#llx, %#llx) fence %llu (done %llu): %llu bytes", (unsigned long long)vk_exec_presented,
           what, (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)fence,
           (unsigned long long)vk_sync_fence_done(), (unsigned long long)bytes);
}
