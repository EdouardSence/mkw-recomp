// MKW_GPU_GUARD=1: checks that stop with rt_fatal, naming the object, before something that would fault on the GPU
// reaches it (step 00). A GPU page fault resets amdgpu and can take the whole desktop down; a
// fatal here only ends the run. The faults measured (31/10-08/11/2026) were reads at address 0 (SQC: a descriptor or
// pointer that is null) and texture reads (TCP) with a permission fault in a VA already unmapped, always from the
// replay or present threads. The checks:
// - Object lifetime: a pool's storage, a physical chunk or a texture image remembers the newest submission part that
//   named it while recording (vk_guard_note); destroying it before the GPU reached that part stops. The replay records
//   a few submissions behind the game, so a destruction from the game's thread (vk_pool_fini) can free what the part
//   being recorded already names. A queued submission that resolves an address in a pool finalized before it was
//   recorded stops too (vk_memory.c resolve_locked), instead of binding nothing.
// - The replay's bind cache (B.pipe, B.ds_bound): a pipeline or set it skips binding must be the one the command
//   buffer holds (tdcs1: a diagnostic bound its own pipeline behind the cache, and the game's next dispatch ran it).
// - Buffer device addresses pushed as constants (mesh as compute) are not 0.
// Null descriptors already stop always (vk_exec_bind.c bind_resources).
#include <stdint.h>
#include <string.h>
#if defined(__linux__)
#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#endif

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"

int vk_guard = -1;

int vk_guard_on_slow(void) {
    const char *e = cfg_get("MKW_GPU_GUARD");
    int on = e && *e && strcmp(e, "0");
    if (on) rt_log("vk: MKW_GPU_GUARD: object lifetimes, bind cache and pushed addresses checked before submitting");
    __atomic_store_n(&vk_guard, on, __ATOMIC_RELEASE);
    return on;
}

// In the repo (build/mkw next to tools/run/gpu_ok.sh), refuses to open the GPU when that script refuses, whatever
// started the run: on 08/11/2026 three capture replays started by hand under perf, not through replay.sh, went to a
// GPU gpu_ok.sh would have refused, and each took a ring timeout. The player's copy has no such script: no check.
void vk_guard_gpu_ok(void) {
#if defined(__linux__)
    char exe[4096], cmd[4200];
    if (!plat_exe_path(exe, sizeof exe)) return;
    char *slash = strrchr(exe, '/');
    if (!slash) return;
    *slash = 0;
    snprintf(cmd, sizeof cmd, "%s/../tools/run/gpu_ok.sh", exe);
    if (access(cmd, X_OK)) return;
    if (system(cmd)) rt_fatal("vk: tools/run/gpu_ok.sh refuses the GPU (MKW_GPU_TAINTED_OK=1 overrides)");
#endif
}

uint64_t vk_guard_use(void) { return vk_exec_recording_fence() ? vk_sync_part_next() : 0; }

void vk_guard_note(uint64_t *use) {
    uint64_t u = vk_guard_use(), old = __atomic_load_n(use, __ATOMIC_RELAXED);
    while (u > old && !__atomic_compare_exchange_n(use, &old, u, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

// The stack of the call that broke the rule, before rt_fatal (build/mkw addresses: tools/run/sym.py names them).
static void guard_stack(void) {
#if defined(__linux__)
    void *frames[24];
    char line[96];
    int n = snprintf(line, sizeof line, "[host] MKW_GPU_GUARD: stack of thread %u:\n", plat_thread_id());
    if (n > 0) (void)!write(2, line, (size_t)n);
    backtrace_symbols_fd(frames, backtrace(frames, 24), 2);
#endif
}

void vk_guard_dead(const char *what, uint64_t handle, uint64_t lo, uint64_t use) {
    uint64_t done = vk_sync_part_done();
    if (use <= done) return;
    guard_stack();
    rt_fatal("vk: MKW_GPU_GUARD: %s %#llx (guest %#llx) destroyed while submission part %llu that names it is not done "
             "(GPU at part %llu, timeline %llu done, %llu issued)", what, (unsigned long long)handle,
             (unsigned long long)lo, (unsigned long long)use, (unsigned long long)done,
             (unsigned long long)vk_sync_fence_done(), (unsigned long long)vk_sync_issued());
}

static _Thread_local VkPipeline pipe_prev[2];   // MKW_GPU_GUARD_TEST=bind: the pipeline bound before the current one

void vk_guard_tie(const char *what, uint64_t handle, uint64_t lo, uint64_t use, uint64_t tie_part) {
    if (use <= tie_part) return;
    guard_stack();
    rt_fatal("vk: MKW_GPU_GUARD: %s %#llx (guest %#llx) queued for destruction after part %llu, but part %llu names it "
             "(%llu parts submitted, GPU at part %llu; timeline %llu done, %llu issued)", what,
             (unsigned long long)handle, (unsigned long long)lo, (unsigned long long)tie_part, (unsigned long long)use,
             (unsigned long long)(vk_sync_part_next() - 1), (unsigned long long)vk_sync_part_done(),
             (unsigned long long)vk_sync_fence_done(), (unsigned long long)vk_sync_issued());
}

// What this thread bound into each command buffer, as the driver sees it (the rc_* wrappers, called on the replay's
// thread whether or not the recorder thread makes the call). The replay's thread binds into more than its own command
// buffer while recording (the upload batch's ASTC decodes): one entry per command buffer, the least recently used one
// replaced. With 8 replaced in turn, a run's replay thread (uploads, chunks) lost the entry of the command buffer it was
// recording and the guard stopped on nothing (guard-race1): a command buffer without an entry is not checked.
#define GUARD_SETS 4
#define GUARD_CBS 32
#define SET_PUSHED ((VkDescriptorSet)(uintptr_t)1)
typedef struct {
    VkCommandBuffer cb;
    uint64_t used;
    VkPipeline pipe[2];
    VkDescriptorSet set[2][GUARD_SETS];
} GuardCb;
static _Thread_local GuardCb gcb[GUARD_CBS];
static _Thread_local uint64_t gclock;
static uint64_t n_unknown;   // skipped binds checked against no entry (logged)

static GuardCb *guard_find(VkCommandBuffer cb) {
    for (int i = 0; i < GUARD_CBS; i++)
        if (gcb[i].cb == cb) return gcb[i].used = ++gclock, &gcb[i];
    return NULL;
}

static GuardCb *guard_cb(VkCommandBuffer cb) {
    GuardCb *g = guard_find(cb);
    if (g) return g;
    g = &gcb[0];
    for (int i = 1; i < GUARD_CBS; i++)
        if (gcb[i].used < g->used) g = &gcb[i];
    memset(g, 0, sizeof *g);
    g->cb = cb, g->used = ++gclock;
    return g;
}

// A command buffer begins with nothing bound (vk_sync.c and vk_exec_par.c, at each vkBeginCommandBuffer of the
// replay): it gets its entry here, so a bind cache that outlived its command buffer is caught.
void vk_guard_cb_begin(VkCommandBuffer cb) {
    GuardCb *g = guard_cb(cb);
    VkCommandBuffer keep = g->cb;
    memset(g, 0, sizeof *g), g->cb = keep, g->used = ++gclock;
}

static int guard_unknown(void) {
    uint64_t n = __atomic_add_fetch(&n_unknown, 1, __ATOMIC_RELAXED);
    if (!(n & (n - 1))) rt_log("vk: MKW_GPU_GUARD: %llu skipped binds in a command buffer this thread has no entry for "
                                "(not checked)", (unsigned long long)n);
    return 1;
}

static int bp_index(VkPipelineBindPoint bp) { return bp == VK_PIPELINE_BIND_POINT_COMPUTE; }

void vk_guard_bound_pipe(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    GuardCb *g = guard_cb(cb);
    int k = bp_index(bp);
    if (g->pipe[k] && g->pipe[k] != p) pipe_prev[k] = g->pipe[k];
    g->pipe[k] = p;
}

void vk_guard_bound_sets(VkCommandBuffer cb, VkPipelineBindPoint bp, uint32_t first, uint32_t n,
                         const VkDescriptorSet *s) {
    GuardCb *g = guard_cb(cb);
    for (uint32_t i = 0; i < n && first + i < GUARD_SETS; i++) g->set[bp_index(bp)][first + i] = s[i];
}

void vk_guard_pushed(VkCommandBuffer cb, VkPipelineBindPoint bp, uint32_t set) {
    if (set < GUARD_SETS) guard_cb(cb)->set[bp_index(bp)][set] = SET_PUSHED;
}

void vk_guard_pipe_hit(VkCommandBuffer cb, int k, VkPipeline p) {
    const GuardCb *g = guard_find(cb);
    if (!g && guard_unknown()) return;
    if (g->pipe[k] == p) return;
    rt_fatal("vk: MKW_GPU_GUARD: the replay skips binding %s pipeline %#llx, but command buffer %p holds %#llx: bound "
             "behind the bind cache (B.pipe), or the cache outlived its command buffer", k ? "compute" : "graphics",
             (unsigned long long)(uintptr_t)p, (void *)cb, (unsigned long long)(uintptr_t)(g ? g->pipe[k] : VK_NULL_HANDLE));
}

void vk_guard_set_hit(VkCommandBuffer cb, VkPipelineBindPoint bp, uint32_t s, VkDescriptorSet set) {
    VkDescriptorSet want = set ? set : SET_PUSHED;
    const GuardCb *g = guard_find(cb);
    if (!g && guard_unknown()) return;
    if (s >= GUARD_SETS || g->set[bp_index(bp)][s] == want) return;
    rt_fatal("vk: MKW_GPU_GUARD: the replay skips binding set %u (%s %#llx), but command buffer %p holds %#llx: "
             "something bound behind the bind cache (B.ds_bound)", s, set ? "set" : "push",
             (unsigned long long)(uintptr_t)set, (void *)cb,
             (unsigned long long)(uintptr_t)(g ? g->set[bp_index(bp)][s] : VK_NULL_HANDLE));
}

void vk_guard_address(const char *what, uint64_t addr) {
    if (!addr) rt_fatal("vk: MKW_GPU_GUARD: %s pushed as a buffer device address of 0", what);
}

// MKW_GPU_GUARD_TEST (with MKW_GPU_GUARD=1): commits one hazard on purpose, to show the guard stops on it before the
// command buffer is submitted. bind: on the 200th bind cache hit, a pipeline bound behind the cache right before the
// check (tdcs1). free: on the 500th texture lookup while recording, a buffer queued for destruction tied only to the
// parts already sent but used by the part being recorded (the tie c-ser1 hung the GPU with).
int vk_guard_test(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = cfg_get("MKW_GPU_GUARD_TEST");
        mode = !e ? 0 : !strcmp(e, "bind") ? GUARD_TEST_BIND : !strcmp(e, "free") ? GUARD_TEST_FREE : 0;
        if (mode) rt_log("vk: MKW_GPU_GUARD_TEST=%s: a hazard committed on purpose", e);
    }
    return mode;
}

static _Thread_local unsigned test_hits, test_lookups;

void vk_guard_test_bind(VkCommandBuffer cb, int k) {
    if (++test_hits != 200 || !pipe_prev[k]) return;
    vkCmdBindPipeline(cb, k ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_prev[k]);
}

void vk_guard_test_free(uint64_t use) {
    if (!use || use != vk_guard_use() || ++test_lookups != 500) return;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO};
    VkBuffer b;
    VmaAllocation a;
    if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &b, &a, NULL) != VK_SUCCESS) return;
    rt_log("vk: MKW_GPU_GUARD_TEST=free: buffer %#llx queued for destruction with part %llu still recording",
           (unsigned long long)(uintptr_t)b, (unsigned long long)use);
    vk_mem_defer_idle(1);
    vk_mem_defer_use(use);
    vk_defer_buffer(b, a);
    vk_mem_defer_use(0);
    vk_mem_defer_idle(0);
}
