// Fixed seed: MKW_SEED=<number> seeds the game's random generator (sead's, main+0xf0c80, from
// the clock on the console: set seed of recomp/patches-mkw.txt calls seed_tick) and the system's entropy (svc GetInfo
// 11, which seeds
// nn::os::GenerateRandomBytes's TinyMt in the sdk, InitializeRandomImpl sdk+0x14e9f0) a sequence from that number, so
// what the game draws from it comes out the same each run (the title's course is the check, runs
// q-*). Empty (default): the system's random bytes, as on the console.
#include <stdatomic.h>
#include <stdlib.h>

#include "cfg.h"
#include "hacks.h"
#include "host.h"

static _Atomic uint64_t state;

static uint64_t mix(uint64_t z) {   // splitmix64's output function
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// The k-th seed asked for from one call site (lr: its return address, mixed with the creator's for the Random
// constructors, patches.c): a count per site, not one for all, because several threads seed and their order changes
// from run to run (one count for all gave the title another course in 2 of 6 runs, qt2-an16 and qt2-sh1024).
#define SEED_SITES 256
static struct { _Atomic uint64_t lr, n; } sites[SEED_SITES];

uint64_t seed_tick(uint64_t lr) {
    int i = 0;
    for (; i < SEED_SITES; i++) {
        uint64_t have = atomic_load_explicit(&sites[i].lr, memory_order_acquire), none = 0;
        if (have == lr) break;
        if (!have && atomic_compare_exchange_strong(&sites[i].lr, &none, lr)) {
            rt_log("seed: the game's random generator seeded from MKW_SEED=%s at lr %#llx", cfg_value("MKW_SEED"),
                   (unsigned long long)lr);
            break;
        }
        if (!have && none == lr) break;   // another thread took the slot for this site
    }
    uint64_t n = i < SEED_SITES ? atomic_fetch_add_explicit(&sites[i].n, 1, memory_order_relaxed) : 0;
    uint64_t base = strtoull(cfg_value("MKW_SEED"), NULL, 0);
    return mix(base ^ mix(lr) ^ (n + 1) * 0x9e3779b97f4a7c15ull) >> 8;   // a 56-bit tick
}

// The system clock (nn::time GetCurrentTime, ov_system.c) with MKW_SEED: stopped at 1 January 2026, 12:00 UTC, so
// what the game derives from the date and time is the same each run. The title's course and the course map's first
// cursor follow it (tt6a/b: the same world tiles); a clock that went on with the game time was read at another second
// in each run and gave other courses (qv2-base, qv2-base2).
int seed_time(int64_t *out) {
    if (!*cfg_value("MKW_SEED")) return 0;
    static _Atomic int logged;
    if (!atomic_exchange(&logged, 1)) rt_log("seed: the system clock stopped at 2026-01-01 12:00 UTC (MKW_SEED)");
    *out = 1767268800;
    return 1;
}

// splitmix64: a full-period sequence from any seed, enough for a seed (no cryptographic use reaches GetInfo 11 first)
int seed_random(uint64_t sub, uint64_t *out) {
    const char *s = cfg_value("MKW_SEED");
    if (!*s) return 0;
    uint64_t base = strtoull(s, NULL, 0);
    uint64_t z = atomic_fetch_add_explicit(&state, 1, memory_order_relaxed);
    if (!z) rt_log("seed: the system's random entropy comes from MKW_SEED=%s", s);
    *out = mix(base + (z + 1) * 0x9e3779b97f4a7c15ull + sub * 0xbf58476d1ce4e5b9ull);
    return 1;
}
