// Code patch sets (recomp/patches.py, gen.py --patches). The generator lifts both the original and the replacement
// instructions of every patched site, and rt_patch_on[set] picks one when the site runs, so a set is switched at start
// without touching code, and also while the game runs (rt_patch_switch: the frame-limit setting of the menu). The sets,
// their environment switches and defaults, and the patch page come from the game code's tables (gamelib.h, from the
// generated patches.h); without them (C generated without --patches) there are no sets and every site runs the
// original.
//
// The patch page is the first 4 KiB page after main's image, inside the module window memory.c reserved: replacement
// code reaches it with one adrp, as MacNativeGames' data page after main (games/totk/runtime/game_patches.c). Its
// layout belongs to the sets that use it.
// Probe modes never call rt_patches_init: every set stays off.
#include "cfg.h"
#include "platform/platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "gfx.h"
#include "hacks.h"
#include "host.h"

#define OUTPUT_SIZE 0xfd0   // patch page offset of the output size (recomp/patches-mkw.txt, set output)
#define SHADOW_HOST 0xfc0   // patch page offset of host_shadow_rb (set shadow)
#define SEED_HOST 0xfb0     // patch page offset of host_seed_tick (set seed)
                            // 0xfa0: game.c host_frame_update (MKW_FPS)
#define SHADOW_POOL_HOST 0xf90   // patch page offset of host_shadow_pool (set shadow)
#define GUEST_RENDERBUFFER_CREATE 0x7000233358ull   // MKW 1.0.0 main: gsys RenderBuffer creation
#define GUEST_RB_ALLOCATOR_INIT 0x70008eb6b0ull     // render-buffer allocator setup (main+0x37b520 allocates from it)
#define GUEST_RB_POOL_NEW 0x700067aa40ull           // operator new(size, heap, alignment), as the allocator calls it
#define GUEST_RB_POOL_INIT 0x70008eb9dcull          // one pool of that allocator (object, added, slot, 7, ...)
#define GUEST_RANDOM_CTOR_LR 0x7000922088ull        // sead::Random::Random, after its GetSystemTick call
#define GUEST_RANDOM_CTOR2_LR 0x70015585a8ull       // another Random constructor, the same

// The shadow set: MKW_SHADOW_SIZE (Graphics tab) in place of the cascaded shadow map size the game picks (3072 docked,
// 2048 handheld). Only a square buffer of 512 or more is resized: what the site's caller (main+0x22d45c) creates.
static uint32_t shadow_size;
static uint64_t rb_obj, rb_heaps[8];   // the render-buffer allocator and its heap chain (host_shadow_pool)
static int rb_pool_done, rb_pool_ok;
static int shadow_second_pool(Cpu *c);
static void host_shadow_rb(Cpu *c) {
    uint32_t *w = (uint32_t *)(uintptr_t)(c->x[0] + 0x68), *h = (uint32_t *)(uintptr_t)(c->x[0] + 0x88);
    if (shadow_size && *w == *h && *w >= 512) {
        if (shadow_size > 3072 && !rb_pool_done) rb_pool_ok = shadow_second_pool(c);
        // Without the second pool a map over 3072 does not fit and main+0x37b520 reads a null block (issue #48, 4096
        // at 1440 lines: "no free second slot", then that crash after the character choice): the game's size then.
        if (shadow_size <= 3072 || rb_pool_ok) {
            static int logs;   // the game creates it again on every course change (#51: 43 000 lines in one log)
            if (*w != shadow_size && logs++ < 4)
                rt_log("patches: shadow map %ux%u instead of %ux%u (MKW_SHADOW_SIZE)", shadow_size, shadow_size, *w, *h);
            *w = *h = shadow_size;
        }
    }
    rt_call(c, GUEST_RENDERBUFFER_CREATE, 4, c->x[0], c->x[1], c->x[2], c->x[3]);
}

// The render-buffer allocator's pool is sized for the console's 3072 map (3 layers of D16), and the shadow map shares
// it with the scene's targets: with a larger map and full-resolution targets it runs out when the map changes (issue
// #9; build/romfs-res, at 720 lines, hid it on Linux). It cannot just get more memory at its init: the pool past
// ~256 MiB, or a second pool made then, takes bytes the game's later fixed-size heaps count on and it fails at boot
// (l9-d, l9-f, l9-k). And the allocator only adds a pool when full if its config names a heap (+0x30; MKW's is 0) and
// its pool table has a free slot (it has one), from a heap that by then is full (l9-j). So the table gets two slots
// (recomp/patches-mkw.txt) and the init's heap chain is kept here; when the first larger map is made, after boot, the
// second pool comes from the heap of that chain with the most room (l9-l: one keeps ~209 MiB from the title on).
static void host_shadow_pool(Cpu *c) {
    uint64_t cfg = c->x[1];
    rb_obj = c->x[0];
    uint64_t h = *(uint64_t *)(uintptr_t)(cfg + 0x28);
    for (int i = 0; i < 8 && h; i++) rb_heaps[i] = h, h = ((uint64_t *)(uintptr_t)h)[7];  // parent: +0x38
    rt_call(c, GUEST_RB_ALLOCATOR_INIT, 2, rb_obj, cfg);
}

// The second pool as the allocator makes one (main+0x37cdbc..0x37ce2c; its init's own, main+0x8eb85c..0x8eb890).
static int shadow_second_pool(Cpu *c) {
    rb_pool_done = 1;
    uint64_t a[4] = {c->x[0], c->x[1], c->x[2], c->x[3]};   // host_shadow_rb passes them on
    uint64_t size = (3 * 2 * ((uint64_t)shadow_size * shadow_size - 3072ull * 3072) + 0xffff) & ~0xffffull;
    uint64_t **table = (uint64_t **)(uintptr_t)(rb_obj + 0x2a90), heap = 0, room = 0;
    if (!rb_obj || *(int32_t *)(uintptr_t)(rb_obj + 0x2a88) < 2 || (*table)[1]) {
        rt_log("patches: render-buffer allocator has no free second slot; no pool for the %u shadow map: the game's "
               "size instead", shadow_size);
        return 0;
    }
    for (int i = 0; i < 8 && rb_heaps[i]; i++) {
        uint64_t h = rb_heaps[i], *vt = (uint64_t *)(uintptr_t)*(uint64_t *)(uintptr_t)h;
        uint64_t free_ = rt_call(c, vt[16], 1, h);   // sead::Heap::getFreeSize
        if (free_ > room) room = free_, heap = h;
    }
    uint64_t pool = room > size + 0x100000 ? rt_call(c, GUEST_RB_POOL_NEW, 3, 0x58, heap, 8) : 0;
    if (pool) {
        uint32_t count = *(uint32_t *)(uintptr_t)(rb_obj + 0x11c0);
        uint32_t flags = (*(uint8_t *)(uintptr_t)(rb_obj + 0x2a68) & 0x80) ? 0x104 : 4;
        rt_call(c, GUEST_RB_POOL_INIT, 8, pool, 0, 1, 7, count, size, flags, heap);
        (*table)[1] = pool;
    }
    rt_log("patches: render-buffer allocator: %s second pool of %#llx bytes for the %u shadow map (heap %#llx, "
           "%#llx free)", pool ? "a" : "no room for a", (unsigned long long)size, shadow_size,
           (unsigned long long)heap, (unsigned long long)room);
    for (int i = 0; i < 4; i++) c->x[i] = a[i];
    return pool != 0;
}

// The seed set: MKW_SEED (Hacks tab) in place of the clock the game seeds its random generator with (core/hacks/seed.c).
// Random::Random (main+0x922070 and 0x1558598) builds dozens of generators from several threads through one call site
// (hk-s4a/c: the same sites, another course): those two are told apart by their creator, whose return address their
// prologue saved at [sp+8] and [sp].
static void host_seed_tick(Cpu *c) {
    uint64_t lr = c->x[30];
    if (lr == GUEST_RANDOM_CTOR_LR) lr ^= *(const uint64_t *)(uintptr_t)(c->sp + 8) << 1;
    else if (lr == GUEST_RANDOM_CTOR2_LR) lr ^= *(const uint64_t *)(uintptr_t)c->sp << 1;
    c->x[0] = seed_tick(lr);
}

static void seed_set(const RtGame *g) {
    for (int i = 0; i < g->npatch_sets; i++)
        if (!strcmp(g->patch_sets[i].name, "seed")) g->patch_on[i] = *cfg_value("MKW_SEED") != 0;
}

static void shadow_set(const RtGame *g) {
    shadow_size = (uint32_t)atoi(cfg_value("MKW_SHADOW_SIZE"));   // "console": 0
    for (int i = 0; i < g->npatch_sets; i++)
        if (!strcmp(g->patch_sets[i].name, "shadow")) g->patch_on[i] = shadow_size != 0;
}

// The output set: with MKW_RESOLUTION not original, that resolution's lines at 16:9 (720: 1280x720) in place of the
// 2560x1440 the game fixes in code; with original, the window's lines when fewer than 1440 (Adrià 08/11/2026: the
// present shrank 2560x1440 to a 1080-line window with its 2x2 bilinear blit, jagged edges, nit-off; a smaller output
// goes through FSR instead). MKW_OUTPUT_PATCH=0 keeps 2560x1440 at any resolution.
static void output_size(const RtGame *g) {
    const char *res = cfg_value("MKW_RESOLUTION"), *off = cfg_get("MKW_OUTPUT_PATCH");
    int lines = strcmp(res, "original") ? atoi(res) : 0;
    if (!lines) {
        int win = gfx_window_lines();
        if (win >= 360 && win < 1440) lines = win;
    }
    for (int i = 0; i < g->npatch_sets; i++) {
        if (strcmp(g->patch_sets[i].name, "output")) continue;
        if (lines <= 0 || (off && !strcmp(off, "0"))) {
            g->patch_on[i] = 0;   // the page holds no size: the site would read 0x0
            return;
        }
        uint32_t wh[2] = {(uint32_t)(lines * 16 / 9), (uint32_t)lines};
        memcpy((void *)(uintptr_t)(g->patch_page + OUTPUT_SIZE), wh, sizeof wh);
        g->patch_on[i] = 1;
        rt_log("patches: game output %ux%u instead of 2560x1440 (MKW_RESOLUTION %s, window lines %d)", wh[0], wh[1],
               res, gfx_window_lines());
        return;
    }
}

void rt_patches_init(void) {
    const RtGame *g = rt_game();
    if (!g->npatch_sets) return;
    if (!plat_mem_commit(g->patch_page, 0x1000))
        rt_fatal("patches: cannot commit the patch page at %#llx", (unsigned long long)g->patch_page);
    char list[512];
    size_t n = 0;
    list[0] = 0;
    for (int i = 0; i < g->npatch_sets; i++) {
        const RtPatchSet *s = &g->patch_sets[i];
        const char *e = cfg_get(s->env);
        // a set with a row in the settings table takes its default from there; the generated default serves the rest
        g->patch_on[i] = (uint8_t)(cfg_meta(s->env) ? cfg_bool(s->env) : e && *e ? strcmp(e, "0") != 0 : s->on);
        n += (size_t)snprintf(list + n, sizeof list - n, "%s%s %s", i ? ", " : "", s->name, g->patch_on[i] ? "on" : "off");
        if (n >= sizeof list) n = sizeof list - 1;
    }
    rt_log("patches: %s (page %#llx)", list, (unsigned long long)g->patch_page);
    output_size(g);
    shadow_set(g);
    seed_set(g);
    frame_rate_init(g->patch_page);
}

void zstd_patch_hosts(uint64_t page);   // ov_zstd.c

void rt_patch_hosts(void) {
    const RtGame *g = rt_game();
    if (g->npatch_sets) zstd_patch_hosts(g->patch_page);
    if (g->npatch_sets) rt_register_host(g->patch_page + SHADOW_HOST, host_shadow_rb);
    if (g->npatch_sets) rt_register_host(g->patch_page + SHADOW_POOL_HOST, host_shadow_pool);
    if (g->npatch_sets) rt_register_host(g->patch_page + SEED_HOST, host_seed_tick);
    if (g->npatch_sets) frame_rate_host(g->patch_page);
}

void rt_patch_switch(int set, int on) {
    const RtGame *g = rt_game();
    if (set >= 0 && set < g->npatch_sets) __atomic_store_n(&g->patch_on[set], (uint8_t)(on != 0), __ATOMIC_RELAXED);
}
