// GPU breadcrumbs for a device loss (vk.h vk_crumbs_*). VK_AMD_buffer_marker has the command stream write a 32-bit
// value into host memory: at TOP_OF_PIPE once the command processor reaches it, at BOTTOM_OF_PIPE once every command
// before it has finished. Each mark writes its start at the top and its end at the bottom into two words of one
// host-coherent buffer, so after a loss one word names the newest mark the GPU started and the other the newest it
// had finished: the hang lies between them. Words 0 and 1: upload batches (vk_mem_upload.c); words 2 and 3: the
// replay's render passes and dispatches (vk_exec.c, vk_exec_ops.c). A ring of records says what each mark was.
//
// Why: issues #4 and #5 (AMD discrete GPUs, v0.1.2) logged only "upload submit failed (-4)": the queue the loss was
// noticed on, not the work that hung. VK_EXT_device_fault (vk_exec_dstat.c) names an address only for a page fault;
// these marks also cover a timeout. Cost: two small writes per pass or dispatch, none per draw; a barrier already
// precedes each pass and dispatch, so the end-of-pipe write adds no wait. MKW_GPU_CRUMBS=0 turns them off.
// The design follows totk-pc-recomp's vk_crumbs.c (MIT, 710b7f5) and Savage Wind Recomp's GPU checkpoints; the code
// is ours.
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_rec.h"

enum { RING = 4096 };   // records kept: a loss is noticed within a few frames of the hang
typedef struct {
    uint32_t id;
    uint8_t kind, ncolor;
    uint64_t frame;
    uint32_t w, h, d;   // pass: render area and layers; dispatch: groups (0 indirect); upload: w its batch number
    VkFormat color[8], depth;
} Mark;

static PFN_vkCmdWriteBufferMarkerAMD write_marker;
static VkBuffer buf;
static VmaAllocation alloc;
static volatile uint32_t *words;
static Mark ring[RING];
static uint32_t seq;   // marks made; several replay threads (MKW_REPLAY_PAR) take ids at once
static int mode;       // rt_knob: 1 on, 2 "ab" (on in the B blocks, for its A/B)

void vk_crumbs_init(void) {
    VkCtx *c = vk_ctx();
    mode = rt_knob("GPU_CRUMBS", "1");
    if (!c || !mode) return;
    if (!c->has_buffer_marker) {
        rt_log("vk: GPU breadcrumbs unavailable (no VK_AMD_buffer_marker)");
        return;
    }
    write_marker = (PFN_vkCmdWriteBufferMarkerAMD)vkGetDeviceProcAddr(c->device, "vkCmdWriteBufferMarkerAMD");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VmaAllocationInfo info;
    if (!write_marker || vmaCreateBuffer(c->vma, &bi, &ai, &buf, &alloc, &info) != VK_SUCCESS) {
        rt_log("vk: GPU breadcrumbs unavailable (no marker buffer)");
        write_marker = NULL;
        return;
    }
    words = info.pMappedData;
    words[0] = words[1] = words[2] = words[3] = 0;
    rt_log("vk: GPU breadcrumbs on%s: upload batches, render passes and dispatches (MKW_GPU_CRUMBS=0 turns them off)",
           mode == 2 ? " in the B blocks (MKW_AB)" : "");
}

void vk_crumbs_release(void) {
    VkCtx *c = vk_ctx();
    if (buf && c) vmaDestroyBuffer(c->vma, buf, alloc);
    buf = VK_NULL_HANDLE, alloc = NULL, words = NULL, write_marker = NULL;
}

static void mark(VkCommandBuffer cb, uint32_t word, uint32_t id) {
    rec_marker(write_marker, cb, word & 1 ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, buf,
               4 * word, id);
}

uint32_t vk_crumb_begin(VkCommandBuffer cb, int kind, uint32_t w, uint32_t h, uint32_t d, uint32_t ncolor,
                        const VkFormat *color, VkFormat depth) {
    if (!words || !cb || !RT_KNOB_ON(mode)) return 0;
    uint32_t id = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);
    if (!id) id = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);   // 0 means no mark
    Mark *m = &ring[id % RING];
    *m = (Mark){.id = id, .kind = (uint8_t)kind, .ncolor = (uint8_t)(ncolor > 8 ? 8 : ncolor), .w = w, .h = h, .d = d,
                .depth = depth, .frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED)};
    if (color) memcpy(m->color, color, sizeof *color * m->ncolor);
    mark(cb, kind == VK_CRUMB_UPLOAD ? 0 : 2, id);
    return id;
}

void vk_crumb_end(VkCommandBuffer cb, int kind, uint32_t id) {
    if (words && cb && id) mark(cb, kind == VK_CRUMB_UPLOAD ? 1 : 3, id);
}

static void describe(uint32_t id, char *o, size_t size) {
    const Mark *m = &ring[id % RING];
    if (!id) {
        snprintf(o, size, "nothing yet");
        return;
    }
    if (m->id != id) {
        snprintf(o, size, "%u (no record: a later mark took its slot)", id);
        return;
    }
    if (m->kind == VK_CRUMB_UPLOAD) {
        snprintf(o, size, "%u, upload batch %u in frame %llu", id, m->w, (unsigned long long)m->frame);
    } else if (m->kind == VK_CRUMB_DISPATCH) {
        snprintf(o, size, "%u, dispatch in frame %llu: %ux%ux%u groups%s", id, (unsigned long long)m->frame, m->w, m->h,
                 m->d, m->w ? "" : " (indirect)");
    } else {
        size_t n = (size_t)snprintf(o, size, "%u, render pass in frame %llu: %ux%u, %u layers, colour formats", id,
                                    (unsigned long long)m->frame, m->w, m->h, m->d);
        for (int i = 0; i < m->ncolor && n < size; i++) n += (size_t)snprintf(o + n, size - n, "%s%d", i ? "," : " ", m->color[i]);
        if (!m->ncolor && n < size) n += (size_t)snprintf(o + n, size - n, " none");
        if (n < size) snprintf(o + n, size - n, ", depth format %d", m->depth);
    }
}

void vk_crumbs_report(void) {
    static int done;
    if (!words || __atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL)) return;
    static const char *const what[2] = {"upload batches", "replay passes and dispatches"};
    rt_log("vk: GPU breadcrumbs (%u marks made): the hang lies after the last finished mark and up to the last started",
           __atomic_load_n(&seq, __ATOMIC_RELAXED));
    for (int k = 0; k < 2; k++) {
        char a[256], b[256];
        describe(words[2 * k], a, sizeof a);
        describe(words[2 * k + 1], b, sizeof b);
        rt_log("vk:   %s: last started %s", what[k], a);
        rt_log("vk:   %s: last finished %s", what[k], b);
    }
}
