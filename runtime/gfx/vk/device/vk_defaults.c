// Default memory caps from the player's GPU and RAM (vk_defaults.h). A figure measured on the Radeon 660M is a floor or
// the integrated case, never everyone's cap: 768 MiB of tile slots on every GPU left an RTX 4070 SUPER's ground flat
// (#31).
#include "vk_defaults.h"

// Tile slots of the sparse textures (vk_sparse.c). Integrated: 768, the 660M's (1024 hung it with MKW_ASTC_BC1,
// run-race41). Discrete: a quarter of the VRAM in 256 MiB steps, 768 to 3072; the title binds 1 472 MiB uncapped
// (run-sp3), so 8 GB and up hold all of it and a 4 GB card keeps the 660M's figure.
uint64_t vk_default_sparse_mib(int discrete, uint64_t vram_mib) {
    if (!discrete) return 768;
    uint64_t mib = vram_mib / 4 / 256 * 256;
    return mib < 768 ? 768 : mib > 3072 ? 3072 : mib;
}

// What VMA may allocate on an integrated GPU (vk_device.c), system RAM outside the process's cgroup: 4096 at least
// (3072 makes the race black, sp-v0), a quarter of the RAM above 16 GB, in 256 MiB steps.
uint64_t vk_default_mem_limit_mib(uint64_t ram_mib) {
    uint64_t mib = ram_mib / 4 / 256 * 256;
    return mib > 4096 ? mib : 4096;
}
