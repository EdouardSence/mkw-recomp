#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "nvn_api.h"
#include "vk_exec.h"

// ---------------------------------------------------------------- MKW_IND_READ (diagnosis, default 0)
// The arguments indirect draws really used, as the GPU had them: at the end of each pass (nothing writes them inside
// one) a copy of each draw's arguments and count goes to a host buffer, read at the fence. One line per pass and VS:
// draws, draws that drew something, instances summed. Found for the flickering trees (06/11/2026): with the shadow's
// losses at zero (lf2) they still appear for one frame; this tells whether that frame asked for them (a culling
// result) or drew them otherwise.
// MKW_IND_READ=1 every VS; =<hex prefix>[,<prefix>...] those VS only, and then one "indd:" line per draw without a
// count buffer: its instance count and first instance in guest memory at record time (the CPU's, which the shadow
// sync copies) against the GPU's, and how many submissions the game was ahead of the replay.
// MKW_IND_HASH=1 (every frame), =<from>-<to> (those frames only) or =cap (while a capture is written, cap.h) with it;
// outside its frames not even MKW_IND_READ notes draws, so a run is not slowed by them: one "indh:" line per draw with
// a hash of its arguments and of every storage and
// uniform buffer its VS and FS bind (vs0..vs15, vu0..vu17, fs0.., fu0..: stage, storage or uniform, slot), as the GPU
// had them at the pass end; uniform buffers as their pool holds them, which the snapshots of vk_usnap.c may not be. A
// capture's replay against the run that captured it says which input differed in a frame with a cactus
//. MKW_IND_DUMP=<dir> with it: each of those buffers written once per frame and address as
// <dir>/f<frame>-<address>.bin, to see which bytes differ.
// MKW_IND_CHECK=<hex address> (instead of the two above): only the storage buffer bound there is copied, and each
// draw's instance records in it, [first, first + count) of 0x50 bytes (the culling dispatch's matrix and material per
// instance), are checked for one kind (their last word): a draw whose records mix kinds read
// another draw's instances (an "indc:" line each, the first 200, and a count per frame). MKW_IND_CHECK_SETS=1 adds an
// "indk:" line per draw with a hash of its records' set (their order aside), to tell a frame whose draws read other
// instances from its neighbours (qck2: the counts were the same). In the serial replays of
// build/cap/qrace every draw of a clean pass read one kind (1 838 of 1 838).

#define HMAX 24   // MKW_IND_HASH: buffers hashed per draw

typedef struct {
    VkBuffer buf, cbuf;
    VkDeviceSize off, coff;
    uint32_t vs, stride, max, at;   // at: offset of its copy in the host buffer
    uint8_t has_count, fi_at;   // fi_at: offset of firstInstance (16 indexed, 12 not)
    uint32_t cpu_ic, cpu_fi;    // the first draw's in guest memory at record time (MKW_IND_READ with a VS list)
    uint64_t ahead;             // submissions the game made past the one being recorded
    uint64_t ssbo;              // the VS's storage buffer 0 and what backs it (vk_mem_shadow_state)
    int ssbo_state;
    uint8_t cpu_ok;
    uint64_t ga;                // guest address of its arguments (0: not host-backed)
    uint32_t uh[2][18];         // MKW_IND_CHECK_SETS: the VS's and FS's uniform blocks as the snapshot holds them
    uint8_t nres;               // MKW_IND_HASH: its buffers, as indices into the pass's res[] and tags (stage << 7 |
    uint16_t res[HMAX];         // uniform << 6 | slot)
    uint8_t rtag[HMAX];
} IndDraw;

typedef struct { VkBuffer b; VkDeviceSize off; uint64_t addr; uint32_t sz, at; } HRes;   // one copy per pass

typedef struct {
    IndDraw *d;
    int n;
    HRes *res;
    uint64_t frame, fence;
    VkBuffer host;
    VmaAllocation alloc;
    const uint8_t *map;
} IndPass;

static int on = -1, hash_on;
static uint64_t hash_from, hash_to = UINT64_MAX;   // MKW_IND_HASH's frames
static int hash_cap;                                // MKW_IND_HASH=cap
static uint64_t check_addr;                         // MKW_IND_CHECK
static int check_sets;                              // MKW_IND_CHECK_SETS=1: an "indk:" line per draw with its records
static uint64_t c_draws, c_mixed, c_lines, c_frame; // completion thread: MKW_IND_CHECK's counts in frame c_frame
int cap_busy(void);                                 // cap.h
static HRes *hres;                                  // replay thread: the open pass's hashed buffers
static int nhres, chres;
static uint32_t want[16], wmask[16];
static int nwant;
static IndDraw *cur;   // replay thread: the draws of the open pass
static int ncur, ccur;
static uint32_t bytes;

static int enabled(uint32_t vs) {
    if (on < 0) {
        const char *e = cfg_get("MKW_IND_READ");
        on = e && *e && strcmp(e, "0");
        if (on && strcmp(e, "1"))
            for (const char *p = e; *p && nwant < 16; p += *p == ',') {
                char *end;
                unsigned long v = strtoul(p, &end, 16);
                int digits = (int)(end - p);
                if (!digits) break;
                if (digits > 8) digits = 8;
                wmask[nwant] = ~0u << (32 - 4 * digits);
                want[nwant++] = (uint32_t)(v << (32 - 4 * digits));
                p = end;
            }
        const char *h = cfg_get("MKW_IND_HASH"), *ck = cfg_get("MKW_IND_CHECK");
        if (ck && *ck && strcmp(ck, "0")) check_addr = strtoull(ck, NULL, 16), on = 1, h = "1";
        check_sets = check_addr && cfg_get("MKW_IND_CHECK_SETS") && atoi(cfg_get("MKW_IND_CHECK_SETS"));
        if (check_sets) vk_usnap_host_wanted = 1;
        hash_on = on && h && strcmp(h, "0");
        if (hash_on && strchr(h, '-')) hash_from = strtoull(h, NULL, 10), hash_to = strtoull(strchr(h, '-') + 1, NULL, 10);
        hash_cap = hash_on && !strcmp(h, "cap");
        if (on) rt_log("vk: indirect arguments read back at each pass end (MKW_IND_READ=%s)%s", e,
                       hash_on ? ", hashed with the VS's and FS's buffers (MKW_IND_HASH)" : "");
    }
    if (!on) return 0;
    if (hash_on && (vk_exec_presented < hash_from || vk_exec_presented > hash_to || (hash_cap && !cap_busy()))) return 0;
    if (!nwant) return 1;
    for (int i = 0; i < nwant; i++)
        if ((vs & wmask[i]) == want[i]) return 1;
    return 0;
}

// MKW_IND_HASH: a buffer of the draw, copied once per pass.
static void hash_add(IndDraw *d, const p_bind_buf *bb, int tag) {
    VkMemSpan s;
    if (check_addr && bb->addr != check_addr) return;
    if (d->nres == HMAX || !bb->addr || !bb->size || bb->size > (8u << 20) || !vk_mem_resolve(bb->addr, bb->size, &s))
        return;
    int i = 0;
    while (i < nhres && !(hres[i].b == s.buffer && hres[i].off == s.offset && hres[i].sz == bb->size)) i++;
    if (i == nhres) {
        if (nhres == chres) {
            chres = chres ? chres * 2 : 64;
            if (!(hres = realloc(hres, sizeof *hres * (size_t)chres))) rt_fatal("vk: out of memory (MKW_IND_HASH)");
        }
        hres[nhres++] = (HRes){s.buffer, s.offset, bb->addr, (uint32_t)bb->size, bytes};
        bytes += ((uint32_t)bb->size + 3) & ~3u;
    }
    d->res[d->nres] = (uint16_t)i, d->rtag[d->nres++] = (uint8_t)tag;
}

static uint64_t fnv(const uint8_t *p, uint64_t n);

void vk_ind_read_note(VkBuffer buf, VkDeviceSize off, uint32_t stride, uint32_t max, VkBuffer cbuf, VkDeviceSize coff,
                      uint32_t vs, int has_count, int indexed, uint64_t guest, const VkExec *ex) {
    uint64_t ssbo = ex->ssbo[0][0].addr;   // the VS's storage buffer 0
    if (!enabled(vs)) return;
    if (ncur == ccur) {
        ccur = ccur ? ccur * 2 : 64;
        if (!(cur = realloc(cur, sizeof *cur * (size_t)ccur))) rt_fatal("vk: out of memory (MKW_IND_READ)");
    }
    uint32_t span = has_count ? (max - 1) * stride + 20 : 20;
    IndDraw *d = &cur[ncur++];
    *d = (IndDraw){buf, cbuf, off, coff, vs, stride, max, bytes, (uint8_t)has_count, indexed ? 16 : 12, 0, 0, 0, 0};
    if (nwant && guest) {   // the CPU's view while recording: what the shadow sync of this submission copies
        const volatile uint32_t *w = (const volatile uint32_t *)(uintptr_t)guest;
        d->cpu_ic = w[1], d->cpu_fi = w[d->fi_at / 4], d->cpu_ok = 1;
        d->ahead = nvn_submit_count() - gfx_replayed_submits();
        d->ssbo = ssbo, d->ssbo_state = ssbo ? vk_mem_shadow_state(ssbo) : -1;
    }
    bytes += span + (has_count ? 4 : 0);
    bytes = (bytes + 3) & ~3u;
    d->ga = guest;
    if (check_sets)   // the first KiB of each uniform block the draw reads, as bound: its snapshot (vk_usnap.c)
        for (int st = 0; st < 2; st++)
            for (int k = 0; k < 18; k++) {
                const p_bind_buf *u = &ex->ubo[st][k];
                uint64_t n = u->size < 1024 ? u->size : 1024;
                const uint8_t *s = n ? vk_usnap_peek(u->addr, n, 1) : NULL;
                d->uh[st][k] = s ? (uint32_t)fnv(s, n) : n ? 1 : 0;
            }
    if (!hash_on) return;
    for (int st = 0; st < 2; st++) {   // NVN_STAGE_VS 0, NVN_STAGE_FS 1 (vk_draw_internal.h)
        for (int k = 0; k < 16; k++) hash_add(d, &ex->ssbo[st][k], st << 7 | k);
        for (int k = 0; k < 18; k++) hash_add(d, &ex->ubo[st][k], st << 7 | 1 << 6 | k);
    }
}

static uint64_t fnv(const uint8_t *p, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static void pass_done(void *arg) {   // completion thread
    IndPass *p = arg;
    VkCtx *c = vk_ctx();
    vmaInvalidateAllocation(c->vma, p->alloc, 0, VK_WHOLE_SIZE);
    enum { MAXV = 32 };
    struct { uint32_t vs, draws, drew; uint64_t inst, first; } v[MAXV];
    int nv = 0;
    for (int i = 0; i < p->n; i++) {
        const IndDraw *d = &p->d[i];
        uint32_t span = d->has_count ? (d->max - 1) * d->stride + 20 : 20, count = 1;
        if (d->has_count) memcpy(&count, p->map + d->at + span, 4), count = count < d->max ? count : d->max;
        if (check_addr) {
            uint32_t ic = 0, fi = 0;
            memcpy(&ic, p->map + d->at + 4, 4);
            memcpy(&fi, p->map + d->at + d->fi_at, 4);
            if (p->frame != c_frame) {
                if (c_draws) rt_log("indc: f%llu %llu draws checked, %llu mixed", (unsigned long long)c_frame,
                                    (unsigned long long)c_draws, (unsigned long long)c_mixed);
                c_frame = p->frame, c_draws = c_mixed = 0;
            }
            for (int r = 0; r < d->nres && ic; r++) {
                const HRes *h = &p->res[d->res[r]];
                if ((uint64_t)fi * 0x50 + (uint64_t)ic * 0x50 > h->sz) continue;
                const uint8_t *rec = p->map + h->at + (uint64_t)fi * 0x50;
                uint32_t k0, k, other = 0;
                uint64_t set = 0;   // the records whatever their order: a sum of their hashes
                memcpy(&k0, rec + 0x4c, 4);
                for (uint32_t j = 0; j < ic; j++) {
                    memcpy(&k, rec + j * 0x50 + 0x4c, 4), other += k != k0;
                    set += fnv(rec + j * 0x50, 0x50);
                }
                if (check_sets) {
                    char us[2 * 18 * 16];
                    int o = 0;
                    for (int st = 0; st < 2; st++)
                        for (int u = 0; u < 18; u++)
                            if (d->uh[st][u]) o += snprintf(us + o, sizeof us - (size_t)o, " %c%d=%08x", st ? 'f' : 'v', u,
                                                            d->uh[st][u]);
                    rt_log("indk: f%llu vs %08x d%d ic %u set %016llx%s", (unsigned long long)p->frame, d->vs, i, ic,
                           (unsigned long long)set, us);
                    char ws[20 * 9 + 1];   // its first record, word by word: which field a changed set changed
                    for (int w = 0; w < 20; w++) {
                        uint32_t x;
                        memcpy(&x, rec + 4 * w, 4);
                        snprintf(ws + 9 * w, 10, " %08x", x);
                    }
                    rt_log("indw: f%llu vs %08x d%d%s", (unsigned long long)p->frame, d->vs, i, ws);
                }
                c_draws++, c_mixed += other != 0;
                if (other && c_lines++ < 200)
                    rt_log("indc: f%llu vs %08x d%d ic %u fi %u: %u of the records not of kind %u", (unsigned long long)p->frame,
                           d->vs, i, ic, fi, other, k0);
                break;
            }
        } else if (hash_on) {
            uint32_t ic = 0, fi = 0;
            memcpy(&ic, p->map + d->at + 4, 4);
            memcpy(&fi, p->map + d->at + d->fi_at, 4);
            char rs[HMAX * 40];
            int o = 0;
            for (int r = 0; r < d->nres; r++) {
                const HRes *h = &p->res[d->res[r]];
                o += snprintf(rs + o, sizeof rs - (size_t)o, " %c%c%d=%llx+%x:%08llx", d->rtag[r] >> 7 ? 'f' : 'v',
                              d->rtag[r] >> 6 & 1 ? 'u' : 's', d->rtag[r] & 63, (unsigned long long)h->addr, h->sz,
                              (unsigned long long)(fnv(p->map + h->at, h->sz) & 0xffffffffu));
            }
            rt_log("indh: f%llu vs %08x d%d count %u ic0 %u fi0 %u args %016llx at %#llx fence %llu%s",
                   (unsigned long long)p->frame, d->vs, i, count, ic, fi,
                   (unsigned long long)fnv(p->map + d->at, span + (d->has_count ? 4 : 0)), (unsigned long long)d->ga,
                   (unsigned long long)p->fence, rs);
            const char *dd = cfg_get("MKW_IND_DUMP");
            for (int r = 0; dd && *dd && r < d->nres; r++) {
                const HRes *h = &p->res[d->res[r]];
                char path[600];
                snprintf(path, sizeof path, "%s/f%llu-%llx.bin", dd, (unsigned long long)p->frame,
                         (unsigned long long)h->addr);
                FILE *f = fopen(path, "rb");   // the frame's first pass that binds it
                if (f) {
                    fclose(f);
                    continue;
                }
                if ((f = fopen(path, "wb"))) fwrite(p->map + h->at, 1, h->sz, f), fclose(f);
            }
        }
        uint64_t inst = 0, first = 0;
        for (uint32_t k = 0; k < count; k++) {
            uint32_t ic, fi;
            const uint8_t *a = p->map + d->at + (uint64_t)k * d->stride;
            memcpy(&ic, a + 4, 4);   // instanceCount in both layouts
            memcpy(&fi, a + d->fi_at, 4);
            inst += ic, first += ic ? fi : 0;
        }
        if (d->cpu_ok && !d->has_count) {   // one line per draw of the listed VS
            uint32_t ic, fi;
            memcpy(&ic, p->map + d->at + 4, 4);
            memcpy(&fi, p->map + d->at + d->fi_at, 4);
            rt_log("indd: f%llu vs %08x cpu ic %u fi %u gpu ic %u fi %u ahead %llu ssbo %#llx state %d",
                   (unsigned long long)p->frame, d->vs, d->cpu_ic, d->cpu_fi, ic, fi, (unsigned long long)d->ahead,
                   (unsigned long long)d->ssbo, d->ssbo_state);
        }
        int j = 0;
        while (j < nv && v[j].vs != d->vs) j++;
        if (j == nv) {
            if (nv == MAXV) continue;
            v[nv++] = (typeof(v[0])){d->vs, 0, 0, 0, 0};
        }
        v[j].draws++, v[j].drew += inst != 0, v[j].inst += inst, v[j].first += first;
    }
    for (int j = 0; j < nv; j++)
        rt_log("indr: f%llu vs %08x draws %u drew %u inst %llu first %llu", (unsigned long long)p->frame, v[j].vs,
               v[j].draws, v[j].drew, (unsigned long long)v[j].inst, (unsigned long long)v[j].first);
    vmaDestroyBuffer(c->vma, p->host, p->alloc);
    free(p->res);
    free(p->d);
    free(p);
}

// MKW_IND_BARRIER (a measurement, default 0; "ab"): the two barriers MKW_IND_READ puts after a pass with indirect
// draws (every write before -> copy, copy -> host), without the copies. lf3 (06/11/2026): with MKW_IND_READ no tree
// flickered in 810 race frames, lf2 had 7 in the same stretch; this separates its barriers from its copies and delay.
static int ind_barrier = -1;
static _Thread_local int pass_ind;   // the open pass of this thread (MKW_REPLAY_PAR) recorded an indirect draw

void vk_ind_note_pass(void) { pass_ind = 1; }

static void barriers(VkExec *ex) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(ex->cb, &di);
    mb = (VkMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
}

// vk_exec_pass_end, after vkCmdEndRendering: the pass's draws' arguments copied out.
void vk_ind_read_pass_end(VkExec *ex) {
    if (ind_barrier < 0) {
        ind_barrier = rt_knob("IND_BARRIER", "0");
        if (ind_barrier) rt_log("vk: barriers after passes with indirect draws%s (MKW_IND_BARRIER, a measurement)",
                                ind_barrier == 2 ? " in the B blocks" : "");
    }
    int had = pass_ind;
    pass_ind = 0;
    static int last = -1;   // the blocks' edges in the log, to count flicker per block (tools/imagen/pops.py)
    if (ind_barrier == 2 && RT_KNOB_ON(ind_barrier) != last)
        last = RT_KNOB_ON(ind_barrier), rt_log("indb: f%llu barriers %s", (unsigned long long)vk_exec_presented,
                                                last ? "on" : "off");
    if (!ncur) {
        if (had && RT_KNOB_ON(ind_barrier)) barriers(ex);
        return;
    }
    IndPass *p = calloc(1, sizeof *p);
    if (!p) rt_fatal("vk: out of memory (MKW_IND_READ)");
    p->map = vk_staging(bytes, &p->host, &p->alloc);
    p->d = cur, p->n = ncur, p->frame = vk_exec_presented, p->res = hres, p->fence = ex->fence;
    cur = NULL, ncur = ccur = 0;
    int nres = nhres;
    hres = NULL, nhres = chres = 0;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(ex->cb, &di);
    for (int i = 0; i < p->n; i++) {
        const IndDraw *d = &p->d[i];
        uint32_t span = d->has_count ? (d->max - 1) * d->stride + 20 : 20;
        VkBufferCopy cp = {d->off, d->at, span};
        vkCmdCopyBuffer(ex->cb, d->buf, p->host, 1, &cp);
        if (d->has_count) {
            VkBufferCopy cc = {d->coff, d->at + span, 4};
            vkCmdCopyBuffer(ex->cb, d->cbuf, p->host, 1, &cc);
        }
    }
    for (int i = 0; i < nres; i++) {
        VkBufferCopy cs = {p->res[i].off, p->res[i].at, p->res[i].sz};
        vkCmdCopyBuffer(ex->cb, p->res[i].b, p->host, 1, &cs);
    }
    mb = (VkMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
    bytes = 0;
    vk_completion_add(ex->fence, pass_done, p);
}
