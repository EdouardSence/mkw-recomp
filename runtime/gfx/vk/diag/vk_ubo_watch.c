// MKW_UBO_WATCH=<hex address>[,...] (diagnosis): each dispatch or draw that binds a uniform block at one of those
// addresses logs, once per frame and address, the block's first 0x80 bytes as the replay binds them (the snapshot of
// vk_usnap.c, else guest memory) and whether guest memory already holds other bytes ("uw:" lines, the first 20 000).
// Found for the cacti: the culling dispatch reads the camera from the block at 0x2a0729300 and,
// in the frame of a cactus, rewrote every instance record of the same instances (ks1).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_exec.h"

#define WATCH_BYTES 0x80

static uint64_t addrs[4], last[4];
static int naddr = -1, lines;

void vk_ubo_watch(const VkExec *ex, int st) {
    if (naddr < 0) {
        naddr = 0;
        for (const char *p = cfg_get("MKW_UBO_WATCH"); p && *p && naddr < 4; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL)
            addrs[naddr++] = strtoull(p, NULL, 16);
        if (naddr) vk_usnap_host_wanted = 1;
    }
    for (int k = 0; k < 18 && naddr; k++) {
        uint64_t a = ex->ubo[st][k].addr;
        for (int i = 0; i < naddr; i++) {
            if (a != addrs[i] || last[i] == vk_exec_presented + 1 || lines >= 20000) continue;
            last[i] = vk_exec_presented + 1, lines++;
            const uint8_t *s = vk_usnap_peek(a, WATCH_BYTES, 1);
            uint8_t live[WATCH_BYTES];
            mem_copy_out(live, a, WATCH_BYTES);
            const uint8_t *b = s ? s : live;
            char hex[WATCH_BYTES / 4 * 9 + 1];
            for (int w = 0; w < WATCH_BYTES / 4; w++) {
                uint32_t x;
                memcpy(&x, b + 4 * w, 4);
                snprintf(hex + 9 * w, 10, " %08x", x);
            }
            rt_log("uw: f%llu %#llx %s%s", (unsigned long long)vk_exec_presented, (unsigned long long)a,
                   s ? (memcmp(s, live, WATCH_BYTES) ? "snap (guest differs)" : "snap") : "live", hex);
        }
    }
}
