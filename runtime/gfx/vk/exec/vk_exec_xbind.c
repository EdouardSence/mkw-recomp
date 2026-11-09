// MKW_BIND_XF (a measurement for the draw cache, default off): graphics descriptors kept
// across frames by the draw's inputs (the pipeline and the bound-slot bytes it reads, vk_exec_bind.c xb_key). =check
// resolves every draw as always and counts how often the kept storage-buffer and texture descriptors are equal to the
// new ones (the cross-frame hit rate a cache could use); =use takes them on a hit without resolving, skipping the
// per-submission texture storage check too: wrong on purpose, only the upper bound of the time a cache would save.
// Uniform buffers and the support buffer are always resolved (their copies move every submission).
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_draw_internal.h"

#define XB_SLOTS (1u << 15)

typedef struct {
    uint64_t key;
    const VkPipeInfo *info;
    uint32_t nd;
    VkDescSlot *d;
} XbEnt;

static XbEnt *tab;   // replay thread only (graphics draws of the serial replay)
static int mode = -1;
static uint64_t n_draws, n_hit, n_equal, n_diff;

int xb_mode(void) {
    if (mode < 0) {
        const char *e = cfg_get("MKW_BIND_XF");
        mode = !e ? 0 : !strcmp(e, "check") ? 1 : !strcmp(e, "use") ? 2 : 0;
        if (mode) rt_log("vk_exec: graphics descriptors kept across frames, %s (MKW_BIND_XF, a measurement)",
                         mode == 1 ? "compared with each new build" : "taken without resolving on a hit");
        if (mode && !(tab = calloc(XB_SLOTS, sizeof *tab))) rt_fatal("vk: out of memory (MKW_BIND_XF)");
    }
    return mode;
}

static void xb_log(void) {
    if (++n_draws % 3000) return;
    rt_log("vk_exec: MKW_BIND_XF over 3000 draws: %llu found, %llu equal, %llu different", (unsigned long long)n_hit,
           (unsigned long long)n_equal, (unsigned long long)n_diff);
    n_hit = n_equal = n_diff = 0;
}

// The kept descriptors of key, or NULL; counts the draw.
const VkDescSlot *xb_get(uint64_t key, const VkPipeInfo *info, uint32_t nd) {
    xb_log();
    const XbEnt *e = &tab[key & (XB_SLOTS - 1)];
    if (e->key != key || e->info != info || e->nd != nd) return NULL;
    n_hit++;
    return e->d;
}

// After a build: keeps d (keep[i]: descriptor i is kept and compared; the others are resolved every time).
void xb_put(uint64_t key, const VkPipeInfo *info, const VkDescSlot *d, uint32_t nd, const uint8_t *keep,
            const VkDescSlot *had) {
    if (had) {
        int same = 1;
        for (uint32_t i = 0; i < nd && same; i++)
            if (keep[i] && memcmp(&had[i], &d[i], sizeof *d)) same = 0;
        same ? n_equal++ : n_diff++;
    }
    XbEnt *e = &tab[key & (XB_SLOTS - 1)];
    if (e->nd != nd) {
        free(e->d);
        if (!(e->d = malloc(sizeof *d * (nd ? nd : 1)))) rt_fatal("vk: out of memory (MKW_BIND_XF)");
    }
    memcpy(e->d, d, sizeof *d * nd);
    e->key = key, e->info = info, e->nd = nd;
}
