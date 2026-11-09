// What the draw replay's descriptors read from uniform-side data (vk_draw_internal.h): the driver constant bank c0
// (vk_c0_fill), the inline uniform updates of the submission being replayed and the storage-buffer addresses the
// shaders load from user uniform blocks (ubo_ptr). The buffer range limits, read once.
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "tex.h"
#include "vk.h"
#include "vk_draw_internal.h"
#include "vk_exec.h"
#include "platform/platform.h"

static void c0_table(uint8_t *at, const p_bind_buf b[16], int n_slots) {
    for (int r = 0; r < n_slots; r++) {
        uint64_t a = b[r].addr;
        uint32_t n = (uint32_t)b[r].size;
        memcpy(at + 16 * r, &a, 8);
        memcpy(at + 16 * r + 8, &n, 4);
    }
}

// The driver constant bank c0 as GLSLC-compiled shaders read it. Storage-buffer tables {u64 address, u32 size} every
// 16 bytes at 0x110 (vertex), 0x510 (fragment), 0x310 (compute): Ryujinx's GlobalToStorage fallback loads each
// candidate base from here and compares the global pointer against it. Compute: dispatch size at 0x4 (NVN2), the uniform
// buffer table at 0x210 + 16 r. Graphics: the largest point size (Maxwell's limit, GLSLC clamps gl_PointSize to it) at
// 0x610, the base instance at 0x644. Other stages: zeros. Unbound slots read zero, since the replay keeps them zero.
// Layout: nxnative dm/gfx/nvn/nvn_mtl.m (MIT).
void vk_c0_fill(uint8_t bank[VK_C0_BYTES], int stage, const p_bind_buf ssbo[16], const p_bind_buf ubo[16],
                const uint32_t dispatch[3], uint32_t base_instance, const uint8_t *cs_table, const uint64_t *tex,
                const uint64_t *img) {
    memset(bank, 0, VK_C0_BYTES);
    int base = c0_ssbo_base(stage);
    if (base < 0) return;
    // Mesh's table at 0x770 has room for 8 slots in c0 (MKW's mesh programs read slots 0-4).
    c0_table(bank + base, ssbo, base + 0x100 <= (int)VK_C0_BYTES ? 16 : ((int)VK_C0_BYTES - base) / 16);
    if (stage == NVN_STAGE_CS) {
        // NVN2: the group counts at 0x4/0x8/0xc (MKW's compute SASS, 31/10/2026: e0e78a51 indexes groups as
        // ctaid.y * c[0][0x4] + ctaid.x, 1591f2fb divides by c[0][0x4] * 32 for 32-thread groups); NVN1 had them at
        // 0x200, which no MKW compute reads.
        memcpy(bank + VK_C0_DISPATCH, dispatch, 12);
        if (cs_table) {   // NVN2: the program's packed table past its push constants (gfx_shader.c cs_res)
            uint32_t at = 0x10 + 4u * cs_table[4];
            for (int k = 0; k < cs_table[0]; k++, at += 16)
                if (k < 16 && at + 16 <= VK_C0_BYTES) memcpy(bank + at, &ubo[k].addr, 8), memcpy(bank + at + 8, &(uint32_t){(uint32_t)ubo[k].size}, 4);
            for (int k = 0; k < cs_table[1]; k++, at += 16)
                if (k < 16 && at + 16 <= VK_C0_BYTES) memcpy(bank + at, &ssbo[k].addr, 8), memcpy(bank + at + 8, &(uint32_t){(uint32_t)ssbo[k].size}, 4);
            for (int k = 0; k < cs_table[2]; k++, at += 8)
                if (k < 128 && tex && at + 8 <= VK_C0_BYTES) memcpy(bank + at, &tex[k], 8);
            for (int k = 0; k < cs_table[3]; k++, at += 8)
                if (k < 32 && img && at + 8 <= VK_C0_BYTES) memcpy(bank + at, &img[k], 8);
            return;
        }
        c0_table(bank + 0x210, ubo, 16);
        return;
    }
    const float max_point = 2047.0f;   // every graphics stage: the task and mesh tables start past it (0x670, 0x770)
    memcpy(bank + 0x610, &max_point, 4);
    memcpy(bank + 0x644, &base_instance, 4);
}

// The inline uniform updates of the submission being replayed, newest last (a ring of UPD_N). op_update_ub writes them
// into the buffer's GPU copy; guest memory gets them only from the write-back when the submission ends. The terrain
// rewrites its pointer block this way before every draw, all at one address, so ubo_ptr reads the latest update that
// covers the pointer, else memory. Payloads stay in the recorded command buffer for the whole replay.
_Thread_local DrawUPD *dtl_upd;

void vk_exec_draw_upd_seed(const VkExec *ex, const void *const *upd, int n) {
    UPD.fence = ex->fence, UPD.n = 0;
    for (int i = n > UPD_N ? n - UPD_N : 0; i < n; i++) upd_note(ex, upd[i]);
}

// 1 when an update of this submission supplied [a, a+n).
static int upd_read(const VkExec *ex, uint64_t a, void *out, size_t n) {
    if (UPD.fence != ex->fence) return 0;
    for (uint32_t i = UPD.n, end = UPD.n > UPD_N ? UPD.n - UPD_N : 0; i-- > end;) {
        const typeof(UPD.u[0]) *e = &UPD.u[i % UPD_N];
        if (a >= e->hi || a + n <= e->lo) continue;
        if (a < e->lo || a + n > e->hi) return 0;   // partly covered: memory
        memcpy(out, e->data + (a - e->lo), n);
        return 1;
    }
    return 0;
}

// MKW_UBO_PTR_SNAP (default 1; "ab"): the address comes from the block's snapshot (vk_usnap.c), as the shader's other
// reads of that block do, not from guest memory at record time. The game rewrites per-object blocks once it has
// submitted (vk_usnap.c), so a pointer read live could be another object's: the draw read that object's storage
// buffer. A "uptr:" line for each frame with such pointers whose live value differed from the
// copy (the first 2 000), and one every 600 frames with the counts.
int ubo_ptr_snap = -1;
static uint64_t up_n, up_snap, up_diff, up_fdiff, up_frame, up_lines;   // atomics: recording threads

static void ubo_ptr_log(void) {
    uint64_t f = vk_exec_presented, was = __atomic_load_n(&up_frame, __ATOMIC_RELAXED);
    if (f == was || __atomic_exchange_n(&up_frame, f, __ATOMIC_RELAXED) == f) return;
    uint64_t fd = __atomic_exchange_n(&up_fdiff, 0, __ATOMIC_RELAXED);
    if (fd && __atomic_fetch_add(&up_lines, 1, __ATOMIC_RELAXED) < 2000)
        rt_log("uptr: f%llu %llu storage addresses differ in guest memory from the snapshot", (unsigned long long)was,
               (unsigned long long)fd);
    if (f / 600 == was / 600) return;
    uint64_t n = __atomic_exchange_n(&up_n, 0, __ATOMIC_RELAXED), s = __atomic_exchange_n(&up_snap, 0, __ATOMIC_RELAXED),
             d = __atomic_exchange_n(&up_diff, 0, __ATOMIC_RELAXED);
    rt_log("uptr: f%llu storage addresses read from uniform blocks since the last line: %llu, %llu from the snapshot, "
           "%llu of them different in guest memory", (unsigned long long)f, (unsigned long long)n,
           (unsigned long long)s, (unsigned long long)d);
}

// That address as the bound block holds it at this point of the replay; 0 when the block is unbound or too short. The
// block has no size field.
uint64_t ubo_ptr(const VkExec *ex, int st, const GfxShaderRes *r) {
    const p_bind_buf *u = &ex->ubo[st][r->cbuf_slot - 3];
    uint64_t off = (uint64_t)r->handle * 4, a = 0, live = 0;
    VkMemSpan sp;
    if (!u->addr || u->size < off + 8 || upd_read(ex, u->addr + off, &a, 8)) return a;
    if (vk_mem_resolve(u->addr + off, 8, &sp)) memcpy(&live, (const void *)(uintptr_t)(u->addr + off), 8);
    vk_usnap_host_wanted = 1;   // from the next submission on: no shader of MKW reads one (up1)
    const uint8_t *s = ubo_ptr_snap ? vk_usnap_peek(u->addr + off, 8, 1) : NULL;
    __atomic_add_fetch(&up_n, 1, __ATOMIC_RELAXED);
    if (s) {
        memcpy(&a, s, 8);
        __atomic_add_fetch(&up_snap, 1, __ATOMIC_RELAXED);
        if (a != live) __atomic_add_fetch(&up_diff, 1, __ATOMIC_RELAXED), __atomic_add_fetch(&up_fdiff, 1, __ATOMIC_RELAXED);
    }
    ubo_ptr_log();
    return s && RT_KNOB_ON(ubo_ptr_snap) ? a : live;
}
