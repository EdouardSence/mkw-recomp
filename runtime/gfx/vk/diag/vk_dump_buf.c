#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"
#include "vk_dump_internal.h"

// ---- MKW_DUMP_BUF=<addr>:<size>[:<off>:<stride>:<w>:<h>]: after every pass and dispatch that is logged, the GPU
// buffer range is copied to a host buffer; when it differs from the previous copy it goes to buf_<pass>.bin, and
// bufs.txt gets the changed bytes' extent plus, with the grid arguments, one map per dword of the w x h records of
// `stride` bytes at `off` (each record a letter: its value's index in the table under the map).

uint64_t bd_addr, bd_size, bd_off, bd_stride;
uint32_t bd_w, bd_h;
static uint8_t *bd_prev;
FILE *bufs_f;

typedef struct { VkBuffer buf; VmaAllocation alloc; const uint8_t *map; int pass; } BufJob;

static void buf_grid(const uint8_t *m) {
    for (uint32_t dw = 0; dw < bd_stride / 4; dw++) {
        uint32_t vals[52];
        int nv = 0, over = 0;
        fprintf(bufs_f, "  dword %u:\n", dw);
        for (uint32_t y = 0; y < bd_h; y++) {
            char line[260];
            uint32_t x;
            for (x = 0; x < bd_w && x < 256; x++) {
                uint64_t o = bd_off + ((uint64_t)y * bd_w + x) * bd_stride + dw * 4;
                if (o + 4 > bd_size) { line[x] = ' '; continue; }
                uint32_t v;
                memcpy(&v, m + o, 4);
                int k = 0;
                while (k < nv && vals[k] != v) k++;
                if (k == nv) {
                    if (nv < 52) vals[nv++] = v;
                    else { over = 1; line[x] = '?'; continue; }
                }
                line[x] = (char)(k < 26 ? 'a' + k : 'A' + k - 26);
            }
            line[x] = 0;
            fprintf(bufs_f, "    %2u %s\n", y, line);
        }
        for (int k = 0; k < nv; k++) fprintf(bufs_f, "%s%c=%08x", k % 8 ? " " : "    ", k < 26 ? 'a' + k : 'A' + k - 26, vals[k]);
        fprintf(bufs_f, "%s\n", over ? " (more: ?)" : "");
    }
}

static void buf_done(void *arg) {
    BufJob *j = arg;
    vmaInvalidateAllocation(dvma(), j->alloc, 0, VK_WHOLE_SIZE);
    plat_mutex_lock(&img_lock);
    if (bufs_f) {
        uint64_t lo = bd_size, hi = 0;
        for (uint64_t i = 0; i < bd_size; i++)
            if (!bd_prev || j->map[i] != bd_prev[i]) { if (i < lo) lo = i; hi = i + 1; }
        if (hi) {
            char path[600];
            snprintf(path, sizeof path, "%s/buf_%04d.bin", dump_dir, j->pass);
            FILE *f = fopen(path, "wb");
            if (f) fwrite(j->map, 1, bd_size, f), fclose(f);
            fprintf(bufs_f, "after pass %04d: bytes %#llx..%#llx changed\n", j->pass, (unsigned long long)lo,
                    (unsigned long long)hi);
            if (bd_stride) buf_grid(j->map);
            fflush(bufs_f);
            if (!bd_prev) bd_prev = malloc(bd_size);
            if (bd_prev) memcpy(bd_prev, j->map, bd_size);
        }
    }
    plat_mutex_unlock(&img_lock);
    vmaDestroyBuffer(dvma(), j->buf, j->alloc);
    free(j);
}

void buf_snap(VkExec *ex, int pass) {
    if (!bd_size) return;
    VkMemSpan s;
    if (!vk_mem_resolve(bd_addr, bd_size, &s) || s.avail < bd_size) return;
    VK_MEM_PICK(s);   // the GPU's view of the buffer
    BufJob *j = calloc(1, sizeof *j);
    if (!j) rt_fatal("vk_dump: out of memory");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bd_size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(dvma(), &bi, &ai, &j->buf, &j->alloc, &info) != VK_SUCCESS || !info.pMappedData) {
        free(j);
        return;
    }
    j->map = info.pMappedData, j->pass = pass;
    VkMemoryBarrier2 mb[2] = {
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT},
        {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb[0]};
    vkCmdPipelineBarrier2(ex->cb, &di);
    VkBufferCopy c = {s.offset, 0, bd_size};
    vkCmdCopyBuffer(ex->cb, s.buffer, j->buf, 1, &c);
    di.pMemoryBarriers = &mb[1];
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded += 3;
    vk_completion_add(ex->fence, buf_done, j);
}

