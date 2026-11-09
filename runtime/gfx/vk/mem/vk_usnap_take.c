// MKW_USNAP_AT_SUBMIT (default 0; "ab"): the uniform snapshot's bytes taken at the submit, on the game's thread, instead
// of where the replay starts recording the submission (vk_usnap.c). With the replay ~3 submits behind the game, the
// game is already writing the next frame's per-object blocks when the snapshot reads them: an object drawn with
// another's matrix for one frame, the flickering cacti and bushes (16 in 2 400 race frames
// without MKW_SUBMIT_CATCHUP, pop0p). The take collects the submission's uniform ranges as vk_usnap_begin does, merges
// them and copies their bytes; vk_usnap_begin pops it in submit order and copies regions from it. Costs the game's
// thread the walk and the copy (~0.3-1 ms a submission, the snapshot's own cost when it ran there, vk_usnap.c).
// MKW_USNAP_CMP=1 (a measurement, without MKW_USNAP_AT_SUBMIT): taken, but the snapshot still copies guest memory where
// the recording starts; vk_usnap.c counts
// per frame the ranges whose bytes changed in between (the replay drawing with the next frame's uniforms or handles,
// Hookshot's "MKW shadows in bands for a frame"; candados-lectura, the kart's shadow).
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_usnap_int.h"
#include "platform/platform.h"

struct UsnapTake {
    UsnapTake *next;
    uint64_t h0;   // the submission's first handle: a take that does not match its submission is dropped
    int n;
    Rng *rng;      // merged ranges, sorted by lo
    uint64_t *off; // where each range's bytes start in bytes
    int nrng;
    uint8_t *bytes;
};

static UsnapTake *head, *tail;
static int queued;
static PlatMutex lock = PLAT_MUTEX_INIT;

static int cmp_on = -1;

static int mode(void) {
    static int m = -1;
    if (m < 0) {
        m = rt_knob("USNAP_AT_SUBMIT", "0");
        cmp_on = !m && cfg_get("MKW_USNAP_CMP") && atoi(cfg_get("MKW_USNAP_CMP")) > 0;
        if (cmp_on) m = 1, rt_log("vk_usnap: uniform ranges the game rewrites between its submit and the recording "
                                  "counted per frame (MKW_USNAP_CMP)");
    }
    return m;
}

void vk_usnap_take(const uint64_t *handles, int n) {
    int m = mode();
    if (!m || !RT_KNOB_ON(m) || n <= 0) return;
    static _Thread_local RngVec v;
    v.n = 0;
    for (int i = 0; i < n; i++)
        if (handles[i]) usnap_collect(&v, nvn_op_first(handles[i]), 0);
    usnap_sort(&v);
    UsnapTake *t = calloc(1, sizeof *t);
    if (!t) rt_fatal("vk_usnap: out of memory (take)");
    t->h0 = handles[0], t->n = n;
    if (v.n && (!(t->rng = malloc(sizeof *t->rng * (size_t)v.n)) || !(t->off = malloc(sizeof *t->off * (size_t)v.n))))
        rt_fatal("vk_usnap: out of memory (take)");
    uint64_t total = 0;
    for (int i = 0; i < v.n;) {
        uint64_t lo = v.r[i].lo, hi = v.r[i].hi;
        for (i++; i < v.n && v.r[i].lo <= hi; i++)
            if (v.r[i].hi > hi) hi = v.r[i].hi;
        t->rng[t->nrng] = (Rng){lo, hi}, t->off[t->nrng++] = total, total += hi - lo;
    }
    if (total && !(t->bytes = malloc(total))) rt_fatal("vk_usnap: out of memory (take of %llu bytes)", (unsigned long long)total);
    for (int i = 0; i < t->nrng; i++) mem_copy_out(t->bytes + t->off[i], t->rng[i].lo, t->rng[i].hi - t->rng[i].lo);
    plat_mutex_lock(&lock);
    if (tail) tail->next = t;
    else head = t;
    tail = t, queued++;
    plat_mutex_unlock(&lock);
}

int usnap_take_compare(void) { return mode() && cmp_on; }

UsnapTake *usnap_take_pop(const uint64_t *handles, int n) {
    if (!head) return NULL;   // read without the lock: a take queued meanwhile belongs to a later submission
    plat_mutex_lock(&lock);
    UsnapTake *t = head;
    if (t) {
        head = t->next, queued--;
        if (!head) tail = NULL;
    }
    plat_mutex_unlock(&lock);
    if (t && (n <= 0 || t->h0 != handles[0] || t->n != n)) {   // not this submission's: guest memory as before
        static volatile int32_t said;
        if (plat_inc32(&said) <= 3) rt_log("vk_usnap: a submit's copy did not match its submission: left out");
        usnap_take_free(t);
        return NULL;
    }
    return t;
}

const uint8_t *usnap_take_bytes(const UsnapTake *t, uint64_t lo, uint64_t hi) {
    int a = 0, b = t->nrng;   // the last range starting at or before lo
    while (b - a > 1) {
        int m = (a + b) / 2;
        if (t->rng[m].lo <= lo) a = m;
        else b = m;
    }
    if (!t->nrng || t->rng[a].lo > lo || t->rng[a].hi < hi) return NULL;
    return t->bytes + t->off[a] + (lo - t->rng[a].lo);
}

void usnap_take_free(UsnapTake *t) {
    if (!t) return;
    free(t->rng), free(t->off), free(t->bytes), free(t);
}
