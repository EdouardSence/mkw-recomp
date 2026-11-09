// Descriptor sets kept across frames by their contents (MKW_DRAW_CACHE) and the measurement that sized the idea
// (MKW_DRAW_CACHE_PROBE). The replay binds through vk_dcache_get from
// vk_exec_bind.c dc_bind, with the uniform buffers dynamic (vk_pipeline.c dyn_ubos).
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

#define MAX_DESC 1024   // as vk_exec_bind.c

// MKW_DRAW_CACHE_PROBE=1 (a measurement): how many draws find their descriptors already built in an earlier frame,
// counted two ways: as they are (a set cache today) and with the uniform buffers' offsets left out (a cache whose
// uniform buffers are dynamic: the game's per-object uniform blocks move every frame, recdiff.py). A table of key
// hashes kept across frames; vk_exec_draw_probe_log prints the counts.
#define PROBE_SLOTS (1u << 20)
static struct {
    int on;
    uint64_t *seen[2];   // [0] as they are, [1] without uniform buffer offsets
    uint64_t draws, hit[2];
    PlatMutex lock;
} PR = {.on = -1, .lock = PLAT_MUTEX_INIT};

static int probe_put(uint64_t *tab, uint64_t h) {
    h |= 1;   // 0 marks an empty slot
    for (uint32_t k = (uint32_t)(h >> 20) & (PROBE_SLOTS - 1), n = 0; n < PROBE_SLOTS; k = (k + 1) & (PROBE_SLOTS - 1), n++) {
        if (tab[k] == h) return 1;
        if (!tab[k]) return tab[k] = h, 0;
    }
    return 0;   // full: counted as a miss
}

static uint64_t probe_hash(const VkPipeInfo *info, const VkDescSlot *d, uint32_t nd) {
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)(uintptr_t)info->layout;
    const uint8_t *p = (const uint8_t *)d;
    for (size_t i = 0; i < nd * sizeof *d; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

void vk_dcache_probe(const VkPipeInfo *info, const VkDescSlot *d, uint32_t nd) {
    if (PR.on < 0) PR.on = cfg_get("MKW_DRAW_CACHE_PROBE") && atoi(cfg_get("MKW_DRAW_CACHE_PROBE"));
    if (!PR.on) return;
    VkDescSlot tmp[MAX_DESC];
    memcpy(tmp, d, nd * sizeof *d);
    for (uint32_t i = 0, at = 0; i < info->n_bindings; i++) {
        uint32_t cnt = info->bindings[i].count ? info->bindings[i].count : 1;
        if (info->bindings[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            info->bindings[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
            for (uint32_t k = 0; k < cnt; k++) tmp[at + k].buffer.offset = 0;
        at += cnt;
    }
    uint64_t h0 = probe_hash(info, d, nd), h1 = probe_hash(info, tmp, nd);
    plat_mutex_lock(&PR.lock);
    if (!PR.seen[0] && (!(PR.seen[0] = calloc(PROBE_SLOTS, 8)) || !(PR.seen[1] = calloc(PROBE_SLOTS, 8))))
        rt_fatal("vk: out of memory (MKW_DRAW_CACHE_PROBE)");
    PR.draws++;
    PR.hit[0] += (uint64_t)probe_put(PR.seen[0], h0);
    PR.hit[1] += (uint64_t)probe_put(PR.seen[1], h1);
    plat_mutex_unlock(&PR.lock);
}

void vk_exec_draw_probe_log(void) {
    if (PR.on <= 0 || !PR.draws) return;
    rt_log("perf: draw cache probe: %llu draws; descriptors seen before: as they are %.1f %%, uniform buffers dynamic "
           "%.1f %%", (unsigned long long)PR.draws, 100.0 * (double)PR.hit[0] / (double)PR.draws,
           100.0 * (double)PR.hit[1] / (double)PR.draws);
    PR.draws = PR.hit[0] = PR.hit[1] = 0;
}

// ---- MKW_DRAW_CACHE: descriptor sets kept across frames by their contents
// A race frame binds what the last one bound but for its uniform blocks' addresses (recdiff.py): with uniform buffers
// dynamic (vk_pipeline.c dyn_ubos) 96-98 % of the draws find their set made in an earlier frame (MKW_DRAW_CACHE_PROBE,
// build/cap/race), and the draw binds it with its offsets instead of writing and pushing its descriptors. Sets name
// objects by handle, and a destroyed object's handle can return for a new one: when vk_destroy_gen moves, or the table
// fills, the table empties and its pools are destroyed once the GPU is past the submission being recorded.
#define DC_SLOTS (1u << 16)
#define DC_HSLOTS (1u << 18)
#define DC_POOL_SETS 4096u
typedef struct { uint64_t h; VkDescriptorSetLayout sl; uint32_t n; VkDescriptorSet set; VkDescSlot *d; } DcEnt;
static struct {
    DcEnt *tab;
    uint32_t used;
    uint64_t *hset;   // handles the kept sets name (the first two words of each descriptor: buffer and offset, or
                      // sampler and view; a word that is no handle only empties the table needlessly)
    uint32_t hused;
    uint64_t gen;   // vk_destroy_gen when the table was last emptied
    VkDescriptorPool *pools;
    int npools, cpools;
    uint64_t hits, misses, flushes;
    uint64_t epoch;   // emptyings since the start: a set kept from before another one is gone (vk_dcache_epoch)
    PlatMutex lock;
} DCH = {.lock = PLAT_MUTEX_INIT};

typedef struct { VkDescriptorPool *pools; int n; } DcRetire;

static void dc_retire(void *arg) {   // completion thread: the GPU is past every use of these sets
    DcRetire *r = arg;
    for (int i = 0; i < r->n; i++) vkDestroyDescriptorPool(vk_ctx()->device, r->pools[i], NULL);
    free(r->pools);
    free(r);
}

static int dc_hfind(uint64_t h, int add) {   // DCH.lock held
    for (uint32_t k = (uint32_t)((h * 0x9E3779B97F4A7C15ull) >> 46) & (DC_HSLOTS - 1);; k = (k + 1) & (DC_HSLOTS - 1)) {
        if (DCH.hset[k] == h) return 1;
        if (!DCH.hset[k]) {
            if (add) DCH.hset[k] = h, DCH.hused++;
            return 0;
        }
    }
}

// vk_destroy_count.h: a handle a kept set names is being destroyed: the table empties before its next use.
void vk_destroy_note(uint64_t h) {
    if (!h || !__atomic_load_n(&DCH.hset, __ATOMIC_ACQUIRE)) return;   // the cache off: no lock per destruction
    plat_mutex_lock(&DCH.lock);
    if (DCH.hset && dc_hfind(h, 0)) __atomic_add_fetch(&vk_destroy_gen, 1, __ATOMIC_RELAXED);
    plat_mutex_unlock(&DCH.lock);
}

static void dc_flush(uint64_t fence) {   // DCH.lock held
    for (uint32_t i = 0; i < DC_SLOTS; i++) free(DCH.tab[i].d);
    memset(DCH.tab, 0, sizeof *DCH.tab * DC_SLOTS);
    memset(DCH.hset, 0, sizeof *DCH.hset * DC_HSLOTS);
    DCH.used = DCH.hused = 0;
    if (DCH.npools) {
        DcRetire *r = malloc(sizeof *r);
        if (!r) rt_fatal("vk: out of memory (draw cache)");
        *r = (DcRetire){DCH.pools, DCH.npools};
        vk_completion_add(fence, dc_retire, r);
        DCH.pools = NULL, DCH.npools = DCH.cpools = 0;
    }
    DCH.flushes++;
    __atomic_add_fetch(&DCH.epoch, 1, __ATOMIC_RELEASE);
}

uint64_t vk_dcache_epoch(void) { return __atomic_load_n(&DCH.epoch, __ATOMIC_ACQUIRE); }

static VkDescriptorSet dc_alloc(VkDescriptorSetLayout sl) {   // DCH.lock held
    VkDevice dev = vk_ctx()->device;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (DCH.npools) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = DCH.pools[DCH.npools - 1], .descriptorSetCount = 1,
                                              .pSetLayouts = &sl};
            VkDescriptorSet set;
            VkResult res = vkAllocateDescriptorSets(dev, &ai, &set);
            if (res == VK_SUCCESS) return set;
            if (res != VK_ERROR_OUT_OF_POOL_MEMORY && res != VK_ERROR_FRAGMENTED_POOL)
                rt_fatal("vk: draw cache set allocation failed (%d)", (int)res);
        }
        static const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, DC_POOL_SETS * 8}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, DC_POOL_SETS * 2},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, DC_POOL_SETS * 2}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DC_POOL_SETS * 4},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, DC_POOL_SETS}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, DC_POOL_SETS / 2},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, DC_POOL_SETS / 8}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, DC_POOL_SETS / 8}};
        VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = DC_POOL_SETS,
                                         .poolSizeCount = sizeof sizes / sizeof sizes[0], .pPoolSizes = sizes};
        if (DCH.npools == DCH.cpools) {
            DCH.cpools = DCH.cpools ? DCH.cpools * 2 : 8;
            if (!(DCH.pools = realloc(DCH.pools, sizeof *DCH.pools * (size_t)DCH.cpools))) rt_fatal("vk: out of memory (draw cache)");
        }
        if (vkCreateDescriptorPool(dev, &pi, NULL, &DCH.pools[DCH.npools]) != VK_SUCCESS) rt_fatal("vk: draw cache pool failed");
        DCH.npools++;
    }
    rt_fatal("vk: draw cache set allocation failed twice");
    return VK_NULL_HANDLE;
}

// The set of layout sl holding descriptors d[0..n) (uniform buffer offsets already taken out); on a miss one is made
// and write(ctx, set) fills it. fence: the submission being recorded (a table emptied now frees its pools after it).
VkDescriptorSet vk_dcache_get(VkDescriptorSetLayout sl, const VkDescSlot *d, uint32_t n, uint64_t fence,
                              void (*write)(void *ctx, VkDescriptorSet set), void *ctx) {
    // By 64-bit words: byte by byte (FNV) a draw's ~20 descriptors cost ~0.4 us, more than the push they spare (dcp1:
    // sets and pushes 0.38 -> 0.8 us a draw with 98 % found).
    uint64_t h = (uint64_t)(uintptr_t)sl * 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0; i < n; i++)
        for (int j = 0; j < 3; j++) h = (h ^ d[i].w[j]) * 0xBF58476D1CE4E5B9ull, h ^= h >> 31;
    plat_mutex_lock(&DCH.lock);
    if (!DCH.tab) {
        uint64_t *hs = calloc(DC_HSLOTS, sizeof *hs);
        if (!(DCH.tab = calloc(DC_SLOTS, sizeof *DCH.tab)) || !hs) rt_fatal("vk: out of memory (draw cache)");
        __atomic_store_n(&DCH.hset, hs, __ATOMIC_RELEASE);
    }
    uint64_t gen = __atomic_load_n(&vk_destroy_gen, __ATOMIC_RELAXED);
    if (gen != DCH.gen || DCH.used >= DC_SLOTS / 4 * 3 || DCH.hused + 2 * n >= DC_HSLOTS / 4 * 3)
        dc_flush(fence), DCH.gen = gen;
    uint32_t k = (uint32_t)(h >> 16) & (DC_SLOTS - 1);
    for (; DCH.tab[k].set; k = (k + 1) & (DC_SLOTS - 1))
        if (DCH.tab[k].h == h && DCH.tab[k].sl == sl && DCH.tab[k].n == n && !memcmp(DCH.tab[k].d, d, n * sizeof *d)) {
            DCH.hits++;
            VkDescriptorSet set = DCH.tab[k].set;
            plat_mutex_unlock(&DCH.lock);
            return set;
        }
    VkDescriptorSet set = dc_alloc(sl);
    write(ctx, set);
    VkDescSlot *copy = malloc(n * sizeof *d + 1);
    if (!copy) rt_fatal("vk: out of memory (draw cache)");
    memcpy(copy, d, n * sizeof *d);
    DCH.tab[k] = (DcEnt){h, sl, n, set, copy};
    for (uint32_t i = 0; i < n; i++)
        for (int j = 0; j < 2; j++)
            if (d[i].w[j]) dc_hfind(d[i].w[j], 1);
    DCH.used++, DCH.misses++;
    plat_mutex_unlock(&DCH.lock);
    return set;
}

void vk_exec_draw_cache_log(void) {
    plat_mutex_lock(&DCH.lock);
    uint64_t n = DCH.hits + DCH.misses;
    if (n) rt_log("perf: draw cache: %llu sets bound, %.1f %% found, %llu made, emptied %llu times, %u kept",
                  (unsigned long long)n, 100.0 * (double)DCH.hits / (double)n, (unsigned long long)DCH.misses,
                  (unsigned long long)DCH.flushes, DCH.used);
    DCH.hits = DCH.misses = DCH.flushes = 0;
    plat_mutex_unlock(&DCH.lock);
}
