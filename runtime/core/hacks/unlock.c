// Unlock everything: MKW_UNLOCK_ALL=1 owns every driver and costume, vehicle and sticker and
// reveals the Special Cup in the save the game opens (Play/account.sav), as a save editor does. Ported from
// Ca1nM/mario-kart-world-save-editor (MIT, mkw_core.py unlock_all / set_owned / reveal_special_cup at b12cbe6): each
// category is a length-prefixed list of owned ids, a parallel per-slot state array and, for drivers, one bit per owned
// slot; new items are appended with the state the game gives a new one. Offsets for game data Product.100 (1.0.0).
// fs_host.c edits the save's working copy when the game opens it: the game keeps the change only if it saves.
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "hacks.h"
#include "host.h"
#include "unlock.h"

#define SAVE_SIZE 850928u
#define CUP_OPEN 0xF5ACu
#define CUP_OPEN_CAP 50u
#define SPECIAL_CUP_SLOT 15u

typedef struct {
    const char *name;
    const uint32_t *ids;
    const int *n;
    uint32_t list, state, mask, cap, new_state;   // mask 0: none
} Category;

static const Category cats[] = {
    {"drivers", unlock_driver_ids, &unlock_driver_ids_n, 0x29800, 0x683C, 0x6A8, 1000, 3},
    {"vehicles", unlock_vehicle_ids, &unlock_vehicle_ids_n, 0x63F28, 0x5704, 0, 1000, 3},
    {"stickers", unlock_sticker_ids, &unlock_sticker_ids_n, 0x2A7A4, 0x8E0, 0, 5000, 1},
};

static uint32_t rd(const uint8_t *b, uint32_t off) {
    uint32_t v;
    memcpy(&v, b + off, 4);
    return v;
}
static void wr(uint8_t *b, uint32_t off, uint32_t v) { memcpy(b + off, &v, 4); }

// The layout this edit expects: each list's capacity word right before it (mkw_core.py check).
static int layout_ok(const uint8_t *b, uint64_t n) {
    if (n != SAVE_SIZE) return 0;
    for (size_t k = 0; k < sizeof cats / sizeof *cats; k++) {
        const Category *c = &cats[k];
        if (rd(b, c->list - 4) != c->cap || rd(b, c->state - 4) != c->cap) return 0;
        if (c->mask && rd(b, c->mask - 4) != c->cap) return 0;
    }
    return rd(b, CUP_OPEN) == CUP_OPEN_CAP;
}

// Appends the ids the list lacks; returns how many.
static int own_all(uint8_t *b, const Category *c) {
    uint32_t have = 0;
    while (have < c->cap && rd(b, c->list + 4 * have)) have++;
    int added = 0;
    for (int i = 0; i < *c->n && have < c->cap; i++) {
        uint32_t id = c->ids[i], k = 0;
        while (k < have && rd(b, c->list + 4 * k) != id) k++;
        if (k < have) continue;
        wr(b, c->list + 4 * have, id);
        wr(b, c->state + 4 * have, c->new_state);
        if (c->mask) wr(b, c->mask + 4 * (have / 32), rd(b, c->mask + 4 * (have / 32)) | 1u << (have % 32));
        have++, added++;
    }
    return added;
}

int unlock_save(uint8_t *b, uint64_t n) {
    if (!layout_ok(b, n)) {
        rt_log("unlock: account.sav (%llu bytes) is not the layout of 1.0.0: left as it is", (unsigned long long)n);
        return 0;
    }
    char note[160];
    int at = 0, total = 0;
    for (size_t k = 0; k < sizeof cats / sizeof *cats; k++) {
        int a = own_all(b, &cats[k]);
        total += a;
        at += snprintf(note + at, sizeof note - (size_t)at, "%s%s +%d", k ? "," : "", cats[k].name, a);
    }
    uint32_t sc = CUP_OPEN + 4 + 4 * SPECIAL_CUP_SLOT;
    if (!rd(b, sc)) wr(b, sc, 2), total++;
    rt_log("unlock: %s, Special Cup open (MKW_UNLOCK_ALL)", note);
    return total;
}
