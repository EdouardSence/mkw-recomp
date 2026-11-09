// Base traps (step 4): the shadow's merge base of a range the GPU may write is a
// copy of the shadow's bytes when the part is submitted, 1-4 MiB ranges that neither the CPU nor the GPU changes most
// of the time (wbc2), and copying them was ~1 ms a frame on the replay thread, the frame's critical path. Guest memory
// holds the same bytes there, so the whole pages of such a range are made read-only instead (trapped == 3) and stand
// for the base: before anything writes one (a write fault, a thaw for a file read or a write-back, a read trap over
// it, an early-read watch), the hook copies the page into the base's buffer (vk_mem_lazy.c), then it is writable.
// Under frz_lock, like the read traps; the hook takes only its own leaf lock. Linux only: it needs the trap map
// (mem_trap_init), which Windows does not have.
#include "host.h"
#include "memory_int.h"

#define PAGE 0x1000ull

static void (*base_hook)(uint64_t page);   // set once, before the first trap
static int64_t nbase;                      // frz_lock: pages trapped for a base now

void mem_base_hook(void (*fn)(uint64_t page)) { base_hook = fn; }

int64_t mem_base_pages(void) { return __atomic_load_n(&nbase, __ATOMIC_RELAXED); }

// frz_lock held: the page's base copy, then the page back to read-write.
void mem_base_fill_locked(uint64_t p) {
    p &= ~(PAGE - 1);
    if (base_hook) base_hook(p);
    if (!plat_mem_protect(p, PAGE, PLAT_PROT_RW))
        rt_fatal("memory: giving back the base-trapped page %#llx failed", (unsigned long long)p);
    trapped[p / PAGE] = 0;
    nbase--;
}

// The whole read-write pages of [lo, hi) with no other trap become base traps; bit k of bits says page lo + k*PAGE
// was trapped (lo page aligned, hi - lo up to 64 pages a word). 0: no trap map (nothing trapped, bits clear).
int mem_base_trap(uint64_t lo, uint64_t hi, uint64_t *bits) {
    uint64_t n = (hi - lo) / PAGE;
    for (uint64_t k = 0; k < (n + 63) / 64; k++) bits[k] = 0;
    if (!n || !watched(lo, hi - lo) || !mem_trap_init()) return 0;
    plat_mutex_lock(&frz_lock);
    mem_thaw_regions_locked(lo, hi - lo);   // a frozen region is read-only as a whole: its thaw would undo the traps
    for (uint64_t x = lo, y; x < hi; x = y) {
        int st = plat_mem_state(x, hi, &y);
        if (st != PLAT_PROT_RW) continue;
        uint64_t a = x;
        while (a < y) {   // runs of pages with no other trap, one protection call each
            while (a < y && trapped[a / PAGE]) a += PAGE;
            uint64_t b = a;
            while (b < y && !trapped[b / PAGE]) b += PAGE;
            if (a == b) continue;
            for (uint64_t q = a; q < b; q += PAGE) trapped[q / PAGE] = 3;   // before the protection: a fault finds it
            if (plat_mem_protect(a, b - a, PLAT_PROT_READ)) {
                for (uint64_t q = a; q < b; q += PAGE) bits[(q - lo) / PAGE / 64] |= 1ull << ((q - lo) / PAGE % 64);
                nbase += (int64_t)((b - a) / PAGE);
            } else {
                for (uint64_t q = a; q < b; q += PAGE) trapped[q / PAGE] = 0;
            }
            a = b;
        }
    }
    plat_mutex_unlock(&frz_lock);
    return 1;
}

// The pages of [lo, hi) still base-trapped go back to read-write without a copy: their base is no longer needed.
void mem_base_release(uint64_t lo, uint64_t hi) {
    if (!trapped || !__atomic_load_n(&nbase, __ATOMIC_RELAXED)) return;
    plat_mutex_lock(&frz_lock);
    for (uint64_t a = lo; a < hi;) {
        while (a < hi && trapped[a / PAGE] != 3) a += PAGE;
        uint64_t b = a;
        while (b < hi && trapped[b / PAGE] == 3) b += PAGE;
        if (a == b) continue;
        if (!plat_mem_protect(a, b - a, PLAT_PROT_RW))
            rt_fatal("memory: releasing base-trapped %#llx+%#llx failed", (unsigned long long)a,
                     (unsigned long long)(b - a));
        for (uint64_t q = a; q < b; q += PAGE) trapped[q / PAGE] = 0;
        nbase -= (int64_t)((b - a) / PAGE);
        a = b;
    }
    plat_mutex_unlock(&frz_lock);
}
