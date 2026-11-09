// MKW_UPLOAD_TIME=1 (diagnosis): the GPU time of each upload batch (vk_mem_upload.c), from a
// timestamp at its start to one at its end, logged with what it held when it took over 50 ms, and the longest so far.
// Why: the capture replays of 08/11/2026 (xb-*, hang1) lost the GPU in the load, on an upload batch RADV_DEBUG=hang saw
// run past 1 s; amdgpu resets the ring of any job past 2 s (lockup_timeout).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_mem_internal.h"
#include "platform/platform.h"

#define UT_SLOTS 64

#define UT_CALLERS 6

static int on = -1;
static VkQueryPool pool;
static uint8_t busy[UT_SLOTS];
static double max_ms;
static struct { void *at; uint32_t n; } callers[UT_SLOTS][UT_CALLERS];   // who recorded into each timed batch

// Starts timing the batch recorded into cb: its slot, or -1 (off, or every slot taken). Under the upload lock.
int vk_uptime_begin(VkCommandBuffer cb) {
    if (on < 0) {
        const char *e = cfg_get("MKW_UPLOAD_TIME");
        on = e && *e && strcmp(e, "0");
        if (on) {
            VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * UT_SLOTS};
            if (vkCreateQueryPool(vk_ctx()->device, &qi, NULL, &pool) != VK_SUCCESS) on = 0;
            rt_log("vk: MKW_UPLOAD_TIME: upload batches over 50 ms of GPU logged%s", on ? "" : " (no query pool: off)");
        }
    }
    if (!on) return -1;
    int s = 0;
    while (s < UT_SLOTS && busy[s]) s++;
    if (s == UT_SLOTS) return -1;
    busy[s] = 1;
    memset(callers[s], 0, sizeof callers[s]);
    vkCmdResetQueryPool(cb, pool, 2u * (uint32_t)s, 2);
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 2u * (uint32_t)s);
    return s;
}

// A vk_upload_begin of the timed batch from return address at (the callers' table: the first UT_CALLERS seen).
void vk_uptime_call(int slot, void *at) {
    if (slot < 0) return;
    for (int i = 0; i < UT_CALLERS; i++)
        if (callers[slot][i].at == at || !callers[slot][i].at) {
            callers[slot][i].at = at, callers[slot][i].n++;
            return;
        }
}

void vk_uptime_end(VkCommandBuffer cb, int slot) {
    if (slot >= 0) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 2u * (uint32_t)slot + 1);
}

// The batch of slot completed: its time, logged with its staging bytes, ASTC texels and vk_upload_begin calls.
void vk_uptime_done(int slot, uint64_t seq, uint64_t stg, uint64_t texels, uint32_t calls) {
    if (slot < 0) return;
    VkCtx *c = vk_ctx();
    uint64_t t[2];
    VkResult r = vkGetQueryPoolResults(c->device, pool, 2u * (uint32_t)slot, 2, sizeof t, t, sizeof t[0],
                                       VK_QUERY_RESULT_64_BIT);
    busy[slot] = 0;
    if (r != VK_SUCCESS) return;
    double ms = (double)(t[1] - t[0]) * c->props.limits.timestampPeriod / 1e6;
    int longest = ms > max_ms;
    if (longest) max_ms = ms;
    if (ms <= 50.0 && !longest) return;
    char who[512];
    int k = 0;
    for (int i = 0; i < UT_CALLERS && callers[slot][i].at && k < (int)sizeof who; i++) {
        char mod[64] = "?";
        uintptr_t at = (uintptr_t)callers[slot][i].at, base = 0;
        plat_module_of(at, mod, sizeof mod, &base);   // module+offset, as tools/run/sym.py reads it
        k += snprintf(who + k, sizeof who - (size_t)k, " %u from %s+%#llx;", callers[slot][i].n, mod,
                      (unsigned long long)(at - base));
    }
    rt_log("vk: upload batch %llu: %.1f ms GPU%s: %.1f MiB staging, %.1f M ASTC texels, %u recordings:%s",
           (unsigned long long)seq, ms, longest ? " (longest so far)" : "", (double)stg / (1 << 20), (double)texels / 1e6,
           calls, k ? who : " none counted");
}
