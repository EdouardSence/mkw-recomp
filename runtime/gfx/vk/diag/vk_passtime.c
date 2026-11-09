// MKW_PASS_TIMING=1: where the GPU's time goes, by render pass size and compute program. The replay writes a GPU
// timestamp before each pass begins and after it ends, around each dispatch and at the start of each submission; the
// interval up to the next timestamp of the same submission counts for the pass's WxH or the dispatch's program, the
// rest (copies, barriers and the idle wait for the next part of the submission) as "outside". A log line every 600 presents gives ms per frame for the
// largest groups. A measurement: two timestamps and a query reset per pass. Off, its only cost is a branch.
//
// MKW_PASS_TIMING=draw also marks each draw and dispatch, inside passes too, and sums the interval up to the next mark
// by program pair (vertex or first mesh stage, fragment; compute alone): the log names the 40 costliest by their cache
// keys (build/spirv/<key>.glsl). The end-of-pipe timestamps overlap draws, so a draw's interval is its share of the
// time the GPU spent finishing it. The slots are reset on the host after each read (hostQueryReset), as a reset command
// cannot go inside a pass.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "platform/platform.h"

#define PT_N 65536    // query ring: ~300 marks a race frame (~3 300 marking draws), read long before it comes back
#define PT_KEYS 256
#define PT_PAIRS 8192

static int pt_on = -1;
static VkQueryPool pt_pool;
static double pt_period;   // ns per tick
static uint64_t pt_mask;
static uint32_t pt_next, pt_first;   // replay thread: next query, first query of the submission being recorded
static uint32_t pt_key[PT_N];        // replay thread writes, completion thread reads after the fence
static PlatMutex pt_lock = PLAT_MUTEX_INIT;   // below
static struct { uint32_t key; uint64_t ns, n; } pt_sum[PT_KEYS];
static uint64_t pt_frames0;
static int pt_host_reset, pt_draws;
static uint32_t pt_dkey[PT_N];       // draw mode: pair index + 1 of the interval a slot opens, 0 for none
static struct { const GfxShader *a, *b; } pt_pair[PT_PAIRS];   // replay thread inserts; slots never change once set
static volatile uint32_t pt_npairs;
static struct { uint64_t ns, n; } pt_psum[PT_PAIRS];           // pt_lock

int vk_pt_on(void) {
    if (pt_on >= 0) return pt_on;
    pt_on = 0;
    const char *e = cfg_get("MKW_PASS_TIMING");
    if (!e || !*e || !strcmp(e, "0")) return 0;
    VkCtx *c = vk_ctx();
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = PT_N};
    if (c->props.limits.timestampPeriod <= 0 || vkCreateQueryPool(c->device, &qi, NULL, &pt_pool) != VK_SUCCESS) {
        rt_log("vk: MKW_PASS_TIMING: no timestamp query pool");
        return 0;
    }
    pt_period = c->props.limits.timestampPeriod;
    // vk_device.c enables every feature the device reports, so supported means enabled.
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    if (f12.hostQueryReset) {
        pt_host_reset = 1;
        vkResetQueryPool(c->device, pt_pool, 0, PT_N);
        pt_draws = !strcmp(e, "draw");
    } else if (!strcmp(e, "draw")) {
        rt_log("vk: MKW_PASS_TIMING=draw needs hostQueryReset: timing by pass size only");
    }
    pt_mask = ~0ull;   // the queue's valid bits: 64 on the devices measured; a wrap shows as one dropped interval
    rt_log("vk: MKW_PASS_TIMING: GPU time by render pass size%s every 600 presents", pt_draws ? " and program pair" : "");
    return pt_on = 1;
}

int vk_pt_enabled(void) { return pt_on > 0; }

// Outside a render pass. key: w << 16 | h of the pass beginning, 0 for anything else.
void vk_pt_mark(VkCommandBuffer cb, uint32_t key) {
    uint32_t q = pt_next++ % PT_N;
    if (!pt_host_reset) vkCmdResetQueryPool(cb, pt_pool, q, 1);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pt_pool, q);
    pt_key[q] = key;
    pt_dkey[q] = 0;
}

int vk_pt_draw_mode(void) { return pt_draws; }

// Replay thread, right before a draw or dispatch is recorded (inside a pass or not).
void vk_pt_draw(VkCommandBuffer cb, const GfxShader *a, const GfxShader *b) {
    uint32_t h = (uint32_t)(((uintptr_t)a * 0x9E3779B97F4A7C15ull ^ (uintptr_t)b * 0xC2B2AE3D27D4EB4Full) >> 40) % PT_PAIRS;
    uint32_t i = h;
    while (pt_pair[i].a && (pt_pair[i].a != a || pt_pair[i].b != b)) {
        i = (i + 1) % PT_PAIRS;
        if (i == h) return;   // full: the draw stays in the previous interval
    }
    if (!pt_pair[i].a) {
        pt_pair[i].b = b;
        __atomic_store_n(&pt_pair[i].a, a, __ATOMIC_RELEASE);
        __atomic_fetch_add(&pt_npairs, 1, __ATOMIC_RELAXED);
    }
    uint32_t q = pt_next % PT_N;
    vk_pt_mark(cb, 0);
    pt_dkey[q] = i + 1;
}

typedef struct { uint32_t first, n; } PtSub;

static void pt_add(uint32_t key, uint64_t ns) {   // pt_lock held
    int i = 0;
    while (i < PT_KEYS && pt_sum[i].n && pt_sum[i].key != key) i++;
    if (i == PT_KEYS) i = PT_KEYS - 1;   // full: the last group takes the rest
    pt_sum[i].key = key, pt_sum[i].ns += ns, pt_sum[i].n++;
}

static void pt_read(void *arg) {   // completion thread, after the submission's fence
    PtSub *s = arg;
    VkCtx *c = vk_ctx();
    uint64_t prev = 0;
    uint32_t pkey = 0, pdkey = 0;
    plat_mutex_lock(&pt_lock);
    for (uint32_t i = 0; i < s->n; i++) {
        uint32_t q = (s->first + i) % PT_N;
        uint64_t t;
        if (vkGetQueryPoolResults(c->device, pt_pool, q, 1, sizeof t, &t, sizeof t, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
            prev = 0;
            continue;
        }
        if (pt_host_reset) vkResetQueryPool(c->device, pt_pool, q, 1);
        t &= pt_mask;
        if (prev && t >= prev) {
            uint64_t ns = (uint64_t)((double)(t - prev) * pt_period);
            if (pdkey) pt_psum[pdkey - 1].ns += ns, pt_psum[pdkey - 1].n++;
            else pt_add(pkey, ns);
        }
        prev = t, pkey = pt_key[q], pdkey = pt_dkey[q];
    }
    plat_mutex_unlock(&pt_lock);
    free(s);
}

void vk_pt_submit_begin(VkCommandBuffer cb) {
    pt_first = pt_next;
    vk_pt_mark(cb, 0);
}

// At the end of a submission's recording, before its last part is ended: the closing mark and the read at its fence.
void vk_pt_submit_end(VkCommandBuffer cb, uint64_t fence) {
    vk_pt_mark(cb, 0);
    PtSub *s = malloc(sizeof *s);
    if (!s) rt_fatal("vk: out of memory");
    s->first = pt_first % PT_N, s->n = pt_next - pt_first;
    vk_completion_add(fence, pt_read, s);
}

static int by_ns(const void *a, const void *b) {
    uint64_t x = ((const typeof(pt_sum[0]) *)a)->ns, y = ((const typeof(pt_sum[0]) *)b)->ns;
    return x < y ? 1 : x > y ? -1 : 0;
}

typedef struct { uint32_t i; uint64_t ns, n; } PtRank;

static int by_rank_ns(const void *a, const void *b) {
    uint64_t x = ((const PtRank *)a)->ns, y = ((const PtRank *)b)->ns;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void hex_key(char *out, const GfxShader *s) {   // the cache key in hex, or "-"
    if (!s) {
        strcpy(out, "-");
        return;
    }
    const uint8_t *k = gfx_shader_key(s);
    for (int j = 0; j < 32; j++) sprintf(out + 2 * j, "%02x", k[j]);
}

// The 40 costliest program pairs since the last line, one log line each, then the sums cleared.
static void pt_pairs_log(uint64_t frame, uint64_t frames) {
    static PtRank r[PT_PAIRS];
    uint32_t n = 0;
    double all = 0;
    plat_mutex_lock(&pt_lock);
    for (uint32_t i = 0; i < PT_PAIRS; i++)
        if (pt_psum[i].n) r[n].i = i, r[n].ns = pt_psum[i].ns, r[n].n = pt_psum[i].n, all += (double)r[n].ns, n++;
    memset(pt_psum, 0, sizeof pt_psum);
    plat_mutex_unlock(&pt_lock);
    qsort(r, n, sizeof r[0], by_rank_ns);
    rt_log("vk: GPU ms per frame by program pair at frame %llu: %.2f in %u pairs (%u known)", (unsigned long long)frame,
           all / 1e6 / frames, n, __atomic_load_n(&pt_npairs, __ATOMIC_RELAXED));
    char ka[65], kb[65];
    for (uint32_t j = 0; j < n && j < 40; j++) {
        const GfxShader *a = __atomic_load_n(&pt_pair[r[j].i].a, __ATOMIC_ACQUIRE), *b = pt_pair[r[j].i].b;
        hex_key(ka, a), hex_key(kb, b);
        rt_log("vk: pair %.3f ms %.1f draws %s %s", r[j].ns / 1e6 / frames, (double)r[j].n / frames, ka, kb);
    }
}

static void pt_log(uint64_t frame, uint64_t frames);

void vk_pt_frame(uint64_t frame) {
    if (frame % 600) return;
    uint64_t frames = pt_frames0 && frame > pt_frames0 ? frame - pt_frames0 : 600;
    pt_frames0 = frame;
    pt_log(frame, frames);
}

// A capture's replay (cap_replay.c) has a few presents, none at a multiple of 600: one line for all of them at its end,
// after the drain, so two replays of one capture compare the GPU time of the same frames (bc1-gpu, 08/11/2026).
void vk_pt_flush(uint64_t frame, uint64_t frames) {
    if (vk_pt_enabled() && frames) pt_log(frame, frames);
}

static void pt_log(uint64_t frame, uint64_t frames) {
    typeof(pt_sum) s;
    plat_mutex_lock(&pt_lock);
    memcpy(s, pt_sum, sizeof s);
    memset(pt_sum, 0, sizeof pt_sum);
    plat_mutex_unlock(&pt_lock);
    qsort(s, PT_KEYS, sizeof s[0], by_ns);
    char buf[2000];
    int k = 0;
    double all = 0, cs = 0, ncs = 0;
    for (int i = 0; i < PT_KEYS && s[i].n; i++) {
        all += (double)s[i].ns;
        if (s[i].key >> 31) cs += (double)s[i].ns, ncs += (double)s[i].n;
    }
    for (int i = 0; i < 22 && s[i].n && k < (int)sizeof buf - 64; i++) {
        if (s[i].key >> 31) k += snprintf(buf + k, sizeof buf - (size_t)k, "; compute %u %.2f (%.1f)", s[i].key & 0x7fffffff,
                                          s[i].ns / 1e6 / frames, (double)s[i].n / frames);
        else if (s[i].key >> 29 == 1) k += snprintf(buf + k, sizeof buf - (size_t)k, "; depth clear %s %ux%u %.2f (%.1f)",
                                                    s[i].key >> 28 & 1 ? "image" : "in pass", (s[i].key >> 14 & 0x3fff) << 4,
                                                    (s[i].key & 0x3fff) << 4, s[i].ns / 1e6 / frames, (double)s[i].n / frames);
        else if (s[i].key >> 30) k += snprintf(buf + k, sizeof buf - (size_t)k, "; op %s %.2f (%.1f)",
                                               nvn_op_name(s[i].key & 0xffff), s[i].ns / 1e6 / frames, (double)s[i].n / frames);
        else if (s[i].key) k += snprintf(buf + k, sizeof buf - (size_t)k, "; %ux%u %.2f (%.1f passes)", s[i].key >> 16,
                                         s[i].key & 0xffff, s[i].ns / 1e6 / frames, (double)s[i].n / frames);
        else k += snprintf(buf + k, sizeof buf - (size_t)k, "; outside %.2f", s[i].ns / 1e6 / frames);
    }
    rt_log("vk: GPU ms per frame by pass size and compute program at frame %llu (all %.2f; compute %.2f in %.1f "
           "dispatches)%s", (unsigned long long)frame, all / 1e6 / frames, cs / 1e6 / frames, ncs / frames, buf);
    if (pt_draws) pt_pairs_log(frame, frames);
}
