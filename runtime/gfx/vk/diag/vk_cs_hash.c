// MKW_CS_HASH=<hex caller>[,...] (diagnosis): each dispatch whose game call site (p_dispatch.caller) is one of those
// logs a hash of every input it reads, once before it runs and its storage buffers again after it: storage buffers and
// indirect arguments copied on the GPU around the dispatch, uniform blocks as the replay binds them (the snapshot of
// vk_usnap.c, else guest memory). One "csh:" line per dispatch and phase. For the cacti: in the
// frame of a cactus the culling dispatch (caller 0x70004613a8, cs e14ec507360d43ac) writes other records for the same
// instances (ks1, ks2); this says which of its inputs differed from the frames around it. Its arguments come from the
// dispatch at 0x7000461784, which can be listed too. Buffers over MKW_CS_HASH_MAX KiB (default 8192) are skipped.
// Each storage buffer's guest memory at record time is hashed too (":g<hash>" when it differs from the GPU's copy),
// to tell a buffer the CPU rewrites from one the GPU wrote. MKW_CS_HASH_DUMP=<dir>: the pre copies written as
// <dir>/f<frame>-<caller>-s<slot>.bin, to see which words changed in a frame.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_draw_internal.h"
#include "vk_exec.h"

#define NCALLER 4
#define NBUF 17   // 16 storage buffers and the indirect arguments

typedef struct {
    uint64_t frame, caller, addr[NBUF], seq;
    uint32_t sz[NBUF], at[NBUF], uh[18], gh[NBUF];
    uint64_t ua[18];
    uint32_t us[18];
    int post;
    VkBuffer host;
    VmaAllocation alloc;
    const uint8_t *map;
} CsCopy;

static uint64_t callers[NCALLER], max_bytes;
static int ncaller = -1;
static uint64_t seq, seq_frame;   // replay thread: dispatches logged in the current frame

static uint64_t fnv(const uint8_t *p, uint64_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

int vk_cs_hash_wanted(uint64_t caller) {
    if (ncaller < 0) {
        ncaller = 0;
        for (const char *p = cfg_get("MKW_CS_HASH"); p && *p && strcmp(p, "0") && ncaller < NCALLER;
             p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL)
            callers[ncaller++] = strtoull(p, NULL, 16);
        const char *m = cfg_get("MKW_CS_HASH_MAX");
        max_bytes = (m && *m ? strtoull(m, NULL, 10) : 8192) << 10;
        if (ncaller) vk_usnap_host_wanted = 1, rt_log("vk: inputs of %d dispatch call sites hashed (MKW_CS_HASH)", ncaller);
    }
    for (int i = 0; i < ncaller; i++)
        if (callers[i] == caller) return 1;
    return 0;
}

static void done(void *arg) {   // completion thread
    CsCopy *c = arg;
    vmaInvalidateAllocation(vk_ctx()->vma, c->alloc, 0, VK_WHOLE_SIZE);
    char s[NBUF * 52 + 18 * 40];
    int o = 0;
    const char *dir = c->post ? NULL : cfg_get("MKW_CS_HASH_DUMP");
    for (int k = 0; k < NBUF; k++) {
        if (!c->sz[k]) continue;
        uint32_t h = (uint32_t)fnv(c->map + c->at[k], c->sz[k]);
        o += snprintf(s + o, sizeof s - (size_t)o, " %s%d=%llx+%x:%08x", k == 16 ? "args" : "s", k == 16 ? 0 : k,
                      (unsigned long long)c->addr[k], c->sz[k], h);
        if (c->gh[k] && c->gh[k] != (h | 1u)) o += snprintf(s + o, sizeof s - (size_t)o, ":g%08x", c->gh[k]);
        if (dir && *dir) {
            char path[600];
            snprintf(path, sizeof path, "%s/f%llu-%llx-s%d.bin", dir, (unsigned long long)c->frame,
                     (unsigned long long)c->caller, k);
            FILE *f = fopen(path, "wb");
            if (f) fwrite(c->map + c->at[k], 1, c->sz[k], f), fclose(f);
        }
    }
    for (int k = 0; k < 18 && !c->post; k++)
        if (c->us[k])
            o += snprintf(s + o, sizeof s - (size_t)o, " u%d=%llx+%x:%08x", k, (unsigned long long)c->ua[k], c->us[k],
                          c->uh[k]);
    rt_log("csh: f%llu caller %#llx n%llu %s%s", (unsigned long long)c->frame, (unsigned long long)c->caller,
           (unsigned long long)c->seq, c->post ? "post" : "pre", s);
    vmaDestroyBuffer(vk_ctx()->vma, c->host, c->alloc);
    free(c);
}

static void barrier(VkExec *ex, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
}

// Around the dispatch, after its resources are bound (outside any pass): post 0 right before vkCmdDispatch*, post 1
// right after. ind: the indirect arguments' guest address (0 for a direct dispatch).
void vk_cs_hash(VkExec *ex, uint64_t caller, uint64_t ind, int post) {
    if (!vk_cs_hash_wanted(caller)) return;
    CsCopy *c = calloc(1, sizeof *c);
    if (!c) rt_fatal("vk: out of memory (MKW_CS_HASH)");
    if (!post) {
        if (seq_frame != vk_exec_presented + 1) seq_frame = vk_exec_presented + 1, seq = 0;
        seq++;
    }
    c->frame = vk_exec_presented, c->caller = caller, c->post = post, c->seq = seq;
    VkMemSpan sp[NBUF];
    uint32_t bytes = 0;
    for (int k = 0; k < NBUF; k++) {
        uint64_t a = k < 16 ? ex->ssbo[NVN_STAGE_CS][k].addr : post ? 0 : ind;
        uint64_t n = k < 16 ? ex->ssbo[NVN_STAGE_CS][k].size : sizeof(VkDispatchIndirectCommand);
        if (!a || !n || n > max_bytes || !vk_mem_resolve(a, n, &sp[k])) continue;
        c->addr[k] = a, c->sz[k] = (uint32_t)n, c->at[k] = bytes;
        bytes += ((uint32_t)n + 3) & ~3u;
        uint8_t *g = post ? NULL : malloc(n);   // the CPU's view while recording
        if (g) mem_copy_out(g, a, n), c->gh[k] = (uint32_t)fnv(g, n) | 1u, free(g);
    }
    for (int k = 0; k < 18 && !post; k++) {   // uniform blocks: the bytes the replay binds, as of the submit call
        const p_bind_buf *u = &ex->ubo[NVN_STAGE_CS][k];
        if (!u->addr || !u->size || u->size > 0x10000) continue;
        const uint8_t *s = vk_usnap_peek(u->addr, u->size, 1);
        uint8_t *live = s ? NULL : malloc(u->size);
        if (!s && live) mem_copy_out(live, u->addr, u->size);
        c->ua[k] = u->addr, c->us[k] = (uint32_t)u->size;
        c->uh[k] = s ? (uint32_t)fnv(s, u->size) : live ? (uint32_t)fnv(live, u->size) ^ 1u : 0;
        free(live);
    }
    c->map = vk_staging(bytes, &c->host, &c->alloc);
    barrier(ex, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    for (int k = 0; k < NBUF; k++)
        if (c->sz[k]) {
            VkBufferCopy cp = {sp[k].offset, c->at[k], c->sz[k]};
            vkCmdCopyBuffer(ex->cb, sp[k].buffer, c->host, 1, &cp);
        }
    // The dispatch (or whatever follows) must not write a buffer before its copy has read it; the host reads at the fence.
    barrier(ex, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vk_completion_add(ex->fence, done, c);
}
