// MKW_REC_DIFF=1 (a measurement for MKW_REUSE): how much of a submission's resolved Vulkan
// commands repeat the previous frame's. Every packet the replay puts in the recorder's ring (vk_rec.c) is hashed with
// the pointers into the packet made relative to it; a "segment" is the packets from one draw or dispatch to the next.
// At the end of each submission its packets and segments are matched against the most similar submission of the ones
// kept from before (as multisets: what reuse at that grain could take), and logged with the packets that did not match
// by command. "loose" also hashes the dynamic offsets and the pushed uniform buffers' buffer and offset as zero: what
// is left once the uniform blocks' addresses are fixed up.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "vk_rec_diff.h"

#define KEEP 12          // submissions kept to match against (a race frame has ~4)
#define NOPS 64

typedef struct {
    uint64_t *pk, *pk_loose, *seg, *seg_loose;   // packet hashes, segment hashes
    uint16_t *op;
    int npk, nseg, cpk, cseg;
} Seq;

static Seq keep[KEEP], cur;
static int nkeep, kept_at;
static uint64_t seg_h, seg_hl;
int rec_diff_on = -1;

static uint64_t mix(uint64_t h, uint64_t w) {
    h ^= w + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

static void grow(void **p, int *cap, int need, size_t el) {
    if (need <= *cap) return;
    *cap = need * 2;
    *p = realloc(*p, (size_t)*cap * el);
    if (!*p) rt_fatal("vk_rec_diff: out of memory");
}

void rec_diff_packet(uint32_t op, const uint64_t *p, uint32_t words, int is_draw, const uint64_t *lo, const uint64_t *hi,
                     const uint32_t *loose_zero, int nloose) {
    uint64_t h = op, hl = op;
    int z = 0;
    for (uint32_t i = 0; i < words; i++) {
        uint64_t w = p[i];
        if (w >= (uint64_t)(uintptr_t)lo && w < (uint64_t)(uintptr_t)hi) w -= (uint64_t)(uintptr_t)p;   // into the packet
        h = mix(h, w);
        while (z < nloose && loose_zero[z] < i) z++;
        hl = mix(hl, z < nloose && loose_zero[z] == i ? 0 : w);
    }
    grow((void **)&cur.pk, &cur.cpk, cur.npk + 1, sizeof *cur.pk);
    cur.pk_loose = realloc(cur.pk_loose, (size_t)cur.cpk * sizeof *cur.pk_loose);
    cur.op = realloc(cur.op, (size_t)cur.cpk * sizeof *cur.op);
    cur.pk[cur.npk] = h, cur.pk_loose[cur.npk] = hl, cur.op[cur.npk] = (uint16_t)op;
    cur.npk++;
    seg_h = mix(seg_h, h), seg_hl = mix(seg_hl, hl);
    if (!is_draw) return;
    grow((void **)&cur.seg, &cur.cseg, cur.nseg + 1, sizeof *cur.seg);
    cur.seg_loose = realloc(cur.seg_loose, (size_t)cur.cseg * sizeof *cur.seg_loose);
    cur.seg[cur.nseg] = seg_h, cur.seg_loose[cur.nseg] = seg_hl;
    cur.nseg++;
    seg_h = seg_hl = 0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// How many of a's n hashes have a partner in b's m (sorted copies, each partner used once). mark: a's matched ones.
static int match(const uint64_t *a, int n, const uint64_t *b, int m, uint8_t *mark) {
    uint64_t *sa = malloc(sizeof *sa * 2 * (size_t)(n + 1)), *sb = malloc(sizeof *sb * (size_t)(m + 1));
    for (int i = 0; i < n; i++) sa[2 * i] = a[i], sa[2 * i + 1] = (uint64_t)i;
    memcpy(sb, b, sizeof *sb * (size_t)m);
    qsort(sa, (size_t)n, 2 * sizeof *sa, cmp_u64);   // pairs sort by their first word
    qsort(sb, (size_t)m, sizeof *sb, cmp_u64);
    int i = 0, j = 0, k = 0;
    if (mark) memset(mark, 0, (size_t)n);
    while (i < n && j < m) {
        if (sa[2 * i] < sb[j]) i++;
        else if (sa[2 * i] > sb[j]) j++;
        else { if (mark) mark[sa[2 * i + 1]] = 1; i++, j++, k++; }
    }
    free(sa), free(sb);
    return k;
}

void rec_diff_begin(void) {
    cur.npk = cur.nseg = 0;
    seg_h = seg_hl = 0;
}

void rec_diff_end(void) {
    if (cur.npk < 64) return;   // small submissions: not the frame's work
    int best = -1, bk = -1;
    for (int s = 0; s < nkeep; s++) {
        int k = match(cur.seg, cur.nseg, keep[s].seg, keep[s].nseg, NULL);
        if (k > bk) bk = k, best = s;
    }
    if (best >= 0) {
        Seq *b = &keep[best];
        uint8_t *mark = malloc((size_t)cur.npk);
        int seg_s = bk, seg_l = match(cur.seg_loose, cur.nseg, b->seg_loose, b->nseg, NULL);
        int pk_l = match(cur.pk_loose, cur.npk, b->pk_loose, b->npk, NULL);
        int pk_s = match(cur.pk, cur.npk, b->pk, b->npk, mark);
        int tot[NOPS] = {0}, miss[NOPS] = {0};
        for (int i = 0; i < cur.npk; i++) tot[cur.op[i] % NOPS]++, miss[cur.op[i] % NOPS] += !mark[i];
        char buf[1200];
        int len = 0;
        for (int o = 0; o < NOPS; o++)
            if (miss[o] && len < (int)sizeof buf - 64)
                len += snprintf(buf + len, sizeof buf - (size_t)len, " %s %d/%d", rec_op_name((uint32_t)o), miss[o], tot[o]);
        rt_log("vk_rec_diff: submission of %d packets, %d segments vs one of %d/%d: segments equal %.1f%% (loose %.1f%%), "
               "packets %.1f%% (loose %.1f%%); unmatched by command:%s", cur.npk, cur.nseg, b->npk, b->nseg,
               100.0 * seg_s / (cur.nseg ? cur.nseg : 1), 100.0 * seg_l / (cur.nseg ? cur.nseg : 1),
               100.0 * pk_s / cur.npk, 100.0 * pk_l / cur.npk, buf);
        free(mark);
    }
    Seq *d = &keep[kept_at];   // the current sequence takes the oldest slot
    Seq t = *d;
    *d = cur;
    cur = t;
    kept_at = (kept_at + 1) % KEEP;
    if (nkeep < KEEP) nkeep++;
}
