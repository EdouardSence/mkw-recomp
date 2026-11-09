#include <stdlib.h>
#include <string.h>

#include "vk_mem_internal.h"

// ---------------------------------------------------------------- shadow merge by bytes (MKW_SHADOW_MERGE)
// A range a submission binds writable is the whole binding, not the bytes the GPU writes: the CPU keeps writing other
// bytes of it (each frame's indirect arguments around the instance counts a culling dispatch adds to). On the console
// both write one memory; here the sync skipped the whole range while it was pending and the write-back copied all of
// it, so the CPU's bytes never reached the GPU and were reverted in guest memory (MKW_SHADOW_LOST, lf1 06/11/2026:
// in a third of the race frames; things drawn by indirect draws appeared or vanished for one frame).
// With a base, the shadow's bytes when the part that writes the range was submitted:
// - sync (merge_in): a CPU-written byte the GPU has not changed (shadow == base) goes into the shadow and the base;
// - write-back (merge_back): only the bytes the GPU changed (shadow != base) go to guest memory.
// Ranges recorded in the part being synced are not skipped at all: the GPU has not run it.
// MKW_SHADOW_CPU_WINS (default 1 when MKW_EARLY_READ waits, as on Linux, else 0; "ab"): a byte both changed goes to the later writer, as on the console, where the GPU
// runs a submission right after it and the CPU then writes the next frame's values: guest memory away from the base
// means the CPU wrote after the part was recorded, so the sync takes it and the write-back leaves it. Without it the
// GPU wins: its write-back reaches guest memory at the fence, late, over the CPU's next values. Measured in the race
// without MKW_SUBMIT_CATCHUP; on build/cap/race it was not measurable, the black
// bush there came from the replay's memory restore.

static int cpu_wins(void) {
    static int m = -1;
    if (m < 0) {
        m = rt_knob("SHADOW_CPU_WINS", mem_early_waits() ? "1" : "0");
        if (m) rt_log("mem: a byte the CPU and the GPU both wrote goes to the later writer%s (MKW_SHADOW_CPU_WINS)",
                      m == 2 ? " in the B blocks" : "");
    }
    return RT_KNOB_ON(m);
}

// The bytes of [lo, hi) of r's shadow, or NULL past MERGE_MAX (s_nobase counts them).
uint8_t *merge_base(const PoolRec *r, uint64_t lo, uint64_t hi) {
    if (!r || !r->smap || lo < r->import_lo || hi > r->import_hi || hi - lo > MERGE_MAX) return NULL;
    uint8_t *b = malloc(hi - lo);
    if (b) memcpy(b, r->smap + (lo - r->import_lo), hi - lo);
    return b;
}

// shadow_lock held: [a, b) of r into the shadow where every base over it still equals the shadow (bases[k] holds
// [blo[k], bhi[k]) and covers [a, b)), updating the bases; the bytes come from src (a submit-time capture,
// vk_mem_capture.c) or, NULL, from guest memory. Returns the bytes copied; merge_over counts those the GPU had changed.
uint64_t merge_over;
uint64_t merge_in(PoolRec *r, uint64_t a, uint64_t b, uint8_t *const *bases, const uint64_t *blo, int n,
                  const uint8_t *src) {
    uint8_t *s = r->smap + (a - r->import_lo);
    uint64_t copied = 0;
    for (uint64_t x = a, y; x < b; x = y) {   // page by page: a trapped page holds the shadow's bytes already
        y = (x & ~0xfffull) + 0x1000;
        if (y > b) y = b;
        if (!src && mem_trapped(x)) continue;
        const uint8_t *g = src ? src + (x - a) : (const uint8_t *)(uintptr_t)x;
        uint8_t *sp = s + (x - a);
        if (!memcmp(g, sp, y - x)) continue;
        for (uint64_t i = 0; i < y - x; i++) {
            if (g[i] == sp[i]) continue;
            int gpu = 0, cpu_after = 1;   // the GPU changed it; the CPU wrote it after the base (it differs from every base)
            for (int k = 0; k < n; k++) {
                uint8_t bk = bases[k][x + i - blo[k]];
                gpu |= bk != sp[i], cpu_after &= g[i] != bk;
            }
            if (gpu && !(cpu_after && cpu_wins())) continue;
            merge_over += (uint64_t)gpu;
            sp[i] = g[i];
            for (int k = 0; k < n; k++) bases[k][x + i - blo[k]] = g[i];
            copied++;
        }
    }
    return copied;
}

// MKW_WB_CENSUS=1 (diagnosis, with MKW_TRACE=perf): merged write-backs by range size, with the span the GPU changed
// (first to last changed byte), the bytes the CPU had changed since the base, the time, and how many were the same
// range as an earlier one of the window: whether bases of whole bindings are needed (sombra-replay 4 and 5).
enum { WBC_BUCKETS = 5, WBC_HASH = 4096 };
static const uint64_t wbc_lim[WBC_BUCKETS] = {4096, 65536, 262144, 1048576, ~0ull};
static struct { uint64_t n, bytes, span, changed, cpu, ns, zero, repeat; } wbc[WBC_BUCKETS];
static uint64_t wbc_seen[WBC_HASH];
static int wbc_on = -1;
static PlatMutex wbc_lock = PLAT_MUTEX_INIT;

static void wbc_note(uint64_t addr, uint64_t size, uint64_t lo, uint64_t hi, uint64_t changed, uint64_t cpu,
                     uint64_t ns) {
    int b = 0;
    while (size > wbc_lim[b]) b++;
    uint64_t key = addr ^ size << 40, h = (key * 0x9E3779B97F4A7C15ull) >> 52;
    plat_mutex_lock(&wbc_lock);
    int rep = 0;
    for (uint64_t k = h;; k = (k + 1) & (WBC_HASH - 1)) {
        if (wbc_seen[k] == key) { rep = 1; break; }
        if (!wbc_seen[k]) { wbc_seen[k] = key; break; }
        if (((k + 1) & (WBC_HASH - 1)) == h) break;
    }
    wbc[b].n++, wbc[b].bytes += size, wbc[b].span += hi > lo ? hi - lo : 0, wbc[b].changed += changed;
    wbc[b].cpu += cpu, wbc[b].ns += ns, wbc[b].zero += !changed, wbc[b].repeat += (uint64_t)rep;
    plat_mutex_unlock(&wbc_lock);
}

void merge_census_log(void) {
    if (wbc_on <= 0) return;
    plat_mutex_lock(&wbc_lock);
    for (int b = 0; b < WBC_BUCKETS; b++)
        if (wbc[b].n)
            rt_log("mem: merged write-backs up to %llu KiB since the last line: %llu (%llu nothing changed, %llu the same "
                   "range again), %.1f MiB, GPU span %.1f MiB, GPU changed %.1f KiB, CPU changed %.1f KiB, %.2f ms",
                   (unsigned long long)(wbc_lim[b] == ~0ull ? MERGE_MAX : wbc_lim[b]) >> 10, (unsigned long long)wbc[b].n,
                   (unsigned long long)wbc[b].zero, (unsigned long long)wbc[b].repeat, wbc[b].bytes / 1048576.0,
                   wbc[b].span / 1048576.0, wbc[b].changed / 1024.0, wbc[b].cpu / 1024.0, wbc[b].ns / 1e6);
    memset(wbc, 0, sizeof wbc);
    memset(wbc_seen, 0, sizeof wbc_seen);
    plat_mutex_unlock(&wbc_lock);
}

// Write-back of [addr, addr+size) from shadow s: the runs the GPU changed against base. Returns the bytes copied.
// With MKW_SHADOW_CPU_WINS a byte the CPU changed since the base (guest memory away from it) stays.
// Guest memory is read only over the runs the GPU changed (watched pages through their protection), a chunk at a time:
// a copy of the whole range for each of ~200 ranges a frame (~44 KiB each, ~100 KiB of them changed) held the fence's
// waiters on the race frame's critical path (tl-cb1: ~1 ms a value in cb_done).
// With a lazy base (vk_mem_lazy.c) each guest page takes its base from lazy_page_base: a page still trapped has none
// copied, and when the shadow equals guest memory there the GPU changed nothing in it.
uint64_t merge_back(uint64_t addr, uint64_t size, const uint8_t *s, const uint8_t *base, LazyBase *lazy) {
    if (wbc_on < 0) wbc_on = rt_knob("WB_CENSUS", "0");
    uint64_t copied = 0, t0 = wbc_on > 0 ? qpc() : 0, lo = ~0ull, hi = 0, gpu = 0;
    uint64_t thawed = addr;   // guest memory below it is thawed already
    int cw = cpu_wins();
    uint8_t g[512];   // guest memory over [k, k + m) of a run, with cpu_wins
    uint8_t tmp[4096];
    const uint8_t *base0 = base;
    uint64_t pend = 0;   // the end of the page of i, as an offset (lazy)
    for (uint64_t i = 0; i < size;) {
        if (lazy && i >= pend) {   // a new guest page: where its base is
            pend = ((addr + i) & ~0xfffull) + 0x1000 - addr;
            if (pend > size) pend = size;
            int r = lazy_page_base(lazy, addr + i, s + i, pend - i, tmp);
            if (!r) { i = pend; continue; }
            base = r == 2 ? (const uint8_t *)((uintptr_t)tmp - i) : base0;   // base[i..pend) is the page's
        }
        // A page at a time first: ranges of 1-4 MiB the GPU binds and never changes are ~460 MiB a 60-frame window of
        // the race (wbc2), and one memcmp call per 64 bytes cost its call each time.
        if (!(i & 4095) && size - i >= 4096 && (!lazy || pend - i >= 4096) && !memcmp(s + i, base + i, 4096)) {
            i += 4096;
            continue;
        }
        if (!(i & 63) && size - i >= 64 && (!lazy || pend - i >= 64) && !memcmp(s + i, base + i, 64)) { i += 64; continue; }
        if (s[i] == base[i]) { i++; continue; }
        uint64_t j = i + 1, jmax = lazy ? pend : size;   // [i, j): the GPU changed every byte
        while (j < jmax && s[j] != base[j]) j++;
        if (i < lo) lo = i;
        hi = j, gpu += j - i;
        for (uint64_t k = i; k < j;) {
            uint64_t m = j - k < sizeof g ? j - k : sizeof g;
            if (cw) mem_copy_out(g, addr + k, m);
            for (uint64_t a = 0; a < m;) {   // runs the CPU left alone (guest memory still at the base)
                if (cw && g[a] != base[k + a]) { a++; continue; }
                uint64_t b = a + 1;
                while (b < m && !(cw && g[b] != base[k + b])) b++;
                // Once a page, not once a run: a range the GPU rewrites (vertex data, ~45 KiB) has thousands of runs
                // broken by bytes that happen to stay equal, and mem_thaw takes frz_lock (wbc2: 154 ms per 60 frames
                // for 277 such ranges, 3.9 MiB changed).
                uint64_t x = addr + k + a, y = x + (b - a);
                if (y > thawed) {
                    uint64_t from = x > thawed ? x & ~0xfffull : thawed;
                    thawed = (y + 0xfff) & ~0xfffull;
                    mem_thaw(from, thawed - from);
                }
                memcpy((void *)(uintptr_t)x, s + k + a, b - a);
                copied += b - a;
                a = b;
            }
            k += m;
        }
        i = j;
    }
    if (wbc_on > 0) wbc_note(addr, size, lo, hi, gpu, gpu - copied, qpc() - t0);
    return copied;
}
