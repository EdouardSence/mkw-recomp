// MKW_TRACE_TL=1 (a measurement; its GPU part needs MKW_TRACE=perf, whose timestamps it reads): the timeline of each
// frame's submissions on one clock. Every 600 frames one line gives, in ms from the frame's first submit: when the game
// submitted its last one, when the replay began and finished recording them, the last vkQueueSubmit, when the GPU began
// and finished (timestamps moved to CLOCK_MONOTONIC with VK_KHR_calibrated_timestamps), when the completion thread saw
// the last value reached and when it released its waiters (side effects such as the shadow write-back run between),
// and the blocking waits on those values (the game's nvnSyncWait among them: it waits each frame for the last frame
// presented, 14-29 ms a frame in ht0, 07/11/2026). What limits the frame shows as the longest step, instead of
// guessing it from per-thread totals.
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "platform/platform.h"

#define TL_RING 8192   // values; a frame takes ~5 (submits and its present)
#define TL_LAG 8       // a frame is read this many presents later, when its values are done

enum { T_SUB, T_RS, T_RE, T_VK, T_REACH, T_DONE, T_G0, T_G1, T_W0, T_W1, T_N };

typedef struct {
    uint64_t f;          // the value this slot holds
    int64_t t[T_N];      // CLOCK_MONOTONIC ns, 0 unset; T_G0/T_W0 the earliest, the others the latest
    int64_t gpu_busy;    // ns of its timed parts
    int64_t cpu0, cpu;   // the replay thread's CPU time when its recording began, and spent recording it
    uint32_t waits;
    uint8_t present;
} TlRec;

static TlRec ring[TL_RING];
static int on = -1;
static int every = 600;   // MKW_TRACE_TL=N (N > 1): a line every N frames; race.sh's race is ~600 frames, and with
                          // MKW_AB a line every 600 split A and B in only one of the two twins (tlab4, tlab5)
static uint64_t pres[TL_LAG + 2];   // the values of the last presents, newest at pres[0]
static uint32_t npres;
static PlatMutex lock = PLAT_MUTEX_INIT;
static PFN_vkGetCalibratedTimestampsKHR p_cal;
static double cal_period;            // ns per GPU tick
static int64_t cal_off;              // CLOCK_MONOTONIC ns = ticks * cal_period + cal_off
static uint64_t cal_at;              // frame of the last calibration
typedef struct { double v[T_N], busy, sub_last, interval, waits, cpu, rt[VK_TL_KINDS]; uint64_t n, g_n, w_n; } TlAcc;
static TlAcc accs[2];   // with MKW_AB: the A and the B frames apart (rt_ab when the frame is read, TL_LAG presents late)
static int64_t prev_s0;

int vk_tl_on(void) {
    if (on < 0) {
        const char *e = cfg_get("MKW_TRACE_TL");
        on = e && atoi(e) ? 1 : 0;
        if (on && atoi(e) > 1) every = atoi(e);
        if (on) rt_log("vk: timeline trace on (MKW_TRACE_TL): one line every %d frames", every);
    }
    return on;
}

static void calibrate(void) {
    VkCtx *c = vk_ctx();
    if (!c->has_calibrated) return;
    if (!p_cal) {
        p_cal = (PFN_vkGetCalibratedTimestampsKHR)vkGetDeviceProcAddr(c->device, "vkGetCalibratedTimestampsKHR");
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(c->phys, &pp);
        cal_period = pp.limits.timestampPeriod;
        if (!p_cal) return;
    }
    // plat_now_ns's clock: CLOCK_MONOTONIC on Linux, QPC ticks on Windows (wtl1: GPU times of -44 million ms with
    // the Linux domain asked there)
#ifdef _WIN32
    VkTimeDomainKHR host = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#else
    VkTimeDomainKHR host = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
#endif
    VkCalibratedTimestampInfoKHR ci[2] = {{.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = VK_TIME_DOMAIN_DEVICE_KHR},
                                          {.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, .timeDomain = host}};
    uint64_t ts[2], dev;
    if (p_cal(c->device, 2, ci, ts, &dev) != VK_SUCCESS) return;
#ifdef _WIN32
    ts[1] = (uint64_t)((double)ts[1] * 1e9 / (double)plat_counter_hz());
#endif
    cal_off = (int64_t)ts[1] - (int64_t)((double)ts[0] * cal_period);
}

static TlRec *rec(uint64_t f) {
    TlRec *r = &ring[f % TL_RING];
    return r->f == f ? r : NULL;
}

static void set(TlRec *r, int k, int64_t t) {
    if (k == T_G0 || k == T_W0) {
        if (!r->t[k] || t < r->t[k]) r->t[k] = t;
    } else if (t > r->t[k]) r->t[k] = t;
}

void vk_tl_mark(uint64_t f, int k, int64_t t) {
    if (vk_tl_on() <= 0 || !f) return;
    plat_mutex_lock(&lock);
    if (k == T_SUB) memset(&ring[f % TL_RING], 0, sizeof ring[0]), ring[f % TL_RING].f = f;
    TlRec *r = rec(f);
    if (r) set(r, k, t);
    plat_mutex_unlock(&lock);
}

void vk_tl_sub(uint64_t f) { vk_tl_mark(f, T_SUB, (int64_t)plat_now_ns()); }
// The replay thread's CPU time (CLOCK_THREAD_CPUTIME_ID): its recording against its span, how long it was off the CPU
// or blocked meanwhile (tlab4i: with chunks and no threads the replay spans 43 ms against 27, the same CPU per window).
static int64_t thread_cpu(void) { return (int64_t)plat_self_cpu_ns(); }

void vk_tl_replay(uint64_t f, int end) {
    if (vk_tl_on() <= 0 || !f) return;
    int64_t now = (int64_t)plat_now_ns(), cpu = thread_cpu();
    plat_mutex_lock(&lock);
    TlRec *r = rec(f);
    if (r) {
        set(r, end ? T_RE : T_RS, now);
        if (!end) r->cpu0 = cpu;
        else if (r->cpu0) r->cpu += cpu - r->cpu0, r->cpu0 = 0;
    }
    plat_mutex_unlock(&lock);
}
void vk_tl_queue_submit(uint64_t f) { vk_tl_mark(f, T_VK, (int64_t)plat_now_ns()); }

// The completion thread: values (from, to] were reached (done: released to their waiters).
void vk_tl_reached(uint64_t from, uint64_t to, int done) {
    if (vk_tl_on() <= 0) return;
    int64_t t = (int64_t)plat_now_ns();
    for (uint64_t f = from + 1; f <= to; f++) vk_tl_mark(f, done ? T_DONE : T_REACH, t);
}

// A timed part of value f: its begin and end timestamps in ticks.
void vk_tl_gpu(uint64_t f, uint64_t s, uint64_t e) {
    if (vk_tl_on() <= 0 || !cal_period) return;
    plat_mutex_lock(&lock);
    TlRec *r = rec(f);
    if (r) {
        set(r, T_G0, (int64_t)((double)s * cal_period) + cal_off);
        set(r, T_G1, (int64_t)((double)e * cal_period) + cal_off);
        r->gpu_busy += (int64_t)((double)(e - s) * cal_period);
    }
    plat_mutex_unlock(&lock);
}

// A blocking wait for value f from t0 to t1.
void vk_tl_wait(uint64_t f, int64_t t0, int64_t t1) {
    if (vk_tl_on() <= 0) return;
    plat_mutex_lock(&lock);
    TlRec *r = rec(f);
    if (r) set(r, T_W0, t0), set(r, T_W1, t1), r->waits++;
    plat_mutex_unlock(&lock);
}

// The frame of values (lo, hi): its first submit is the origin.
static void frame_add(uint64_t lo, uint64_t hi) {
    int64_t s0 = 0, v[T_N] = {0}, busy = 0, sub_last = 0, cpu = 0;
    uint32_t waits = 0;
    for (uint64_t f = lo + 1; f < hi; f++) {
        TlRec *r = rec(f);
        if (!r || !r->t[T_SUB]) continue;
        if (!s0) s0 = r->t[T_SUB];
        sub_last = r->t[T_SUB];
        for (int k = T_RS; k < T_N; k++) {
            if (!r->t[k]) continue;
            if (k == T_RS || k == T_G0 || k == T_W0) { if (!v[k] || r->t[k] < v[k]) v[k] = r->t[k]; }
            else if (r->t[k] > v[k]) v[k] = r->t[k];
        }
        busy += r->gpu_busy, waits += r->waits, cpu += r->cpu;
    }
    if (!s0 || !v[T_DONE]) return;
    TlAcc *a = &accs[rt_ab() ? 1 : 0];
    a->n++;
    a->sub_last += (double)(sub_last - s0);
    a->cpu += (double)cpu;
    for (int k = T_RS; k <= T_DONE; k++) a->v[k] += v[k] ? (double)(v[k] - s0) : 0;
    if (v[T_G0] && v[T_G1]) a->g_n++, a->v[T_G0] += (double)(v[T_G0] - s0), a->v[T_G1] += (double)(v[T_G1] - s0), a->busy += (double)busy;
    if (waits) a->w_n++, a->waits += waits, a->v[T_W0] += (double)(v[T_W0] - s0), a->v[T_W1] += (double)(v[T_W1] - s0);
    if (prev_s0) a->interval += (double)(s0 - prev_s0);
    prev_s0 = s0;
}

// The replay thread's wall time by what it did (VK_TL_*), into the block it happens in: the replay spans ~35-40 ms a
// frame with ~23 of recording CPU (tlab6), and between its submissions it waits for the game or handles other items.
void vk_tl_replay_time(int kind, int64_t ns) {
    if (vk_tl_on() <= 0 || kind < 0 || kind >= VK_TL_KINDS) return;
    plat_mutex_lock(&lock);
    accs[rt_ab() ? 1 : 0].rt[kind] += (double)ns;
    plat_mutex_unlock(&lock);
}

// The replay thread, at each present of value f.
void vk_tl_present(uint64_t f, uint64_t frame) {
    if (vk_tl_on() <= 0) return;
    if (!cal_at || frame >= cal_at + 600) cal_at = frame, calibrate();
    plat_mutex_lock(&lock);
    memmove(pres + 1, pres, sizeof pres - sizeof pres[0]);
    pres[0] = f;
    if (++npres > TL_LAG + 1) frame_add(pres[TL_LAG + 1], pres[TL_LAG]);
    plat_mutex_unlock(&lock);
    if (frame % (uint64_t)every) return;
    for (int k = 0; k < 2; k++) {
        TlAcc *a = &accs[k];
        if (!a->n) continue;
        double n = (double)a->n, g = a->g_n ? (double)a->g_n : 1, w = a->w_n ? (double)a->w_n : 1;
#define MS(x) ((x) / 1e6)
        rt_log("vk: timeline%s at frame %llu, %llu frames, ms from the frame's first submit: last submit %.1f; replay "
               "%.1f-%.1f (CPU %.1f); last vkQueueSubmit %.1f; GPU %.1f-%.1f (busy %.1f, %llu frames timed); reached %.1f, released "
               "%.1f; waits on its values %.1f per frame, from %.1f to %.1f (%llu frames); a frame every %.1f",
               k ? " B" : accs[1].n ? " A" : "", (unsigned long long)frame, (unsigned long long)a->n, MS(a->sub_last / n),
               MS(a->v[T_RS] / n), MS(a->v[T_RE] / n), MS(a->cpu / n), MS(a->v[T_VK] / n), MS(a->v[T_G0] / g), MS(a->v[T_G1] / g),
               MS(a->busy / g), (unsigned long long)a->g_n, MS(a->v[T_REACH] / n), MS(a->v[T_DONE] / n), a->waits / n,
               MS(a->v[T_W0] / w), MS(a->v[T_W1] / w), (unsigned long long)a->w_n, MS(a->interval / (n > 1 ? n - 1 : 1)));
        rt_log("vk: timeline%s at frame %llu: replay thread per frame, ms: idle %.1f, submits %.1f, presents %.1f, "
               "releases %.1f, registrations %.1f", k ? " B" : accs[1].n ? " A" : "", (unsigned long long)frame,
               MS(a->rt[VK_TL_IDLE] / n), MS(a->rt[VK_TL_SUBMIT] / n), MS(a->rt[VK_TL_PRESENT] / n),
               MS(a->rt[VK_TL_RELEASE] / n), MS(a->rt[VK_TL_REG] / n));
#undef MS
    }
    memset(accs, 0, sizeof accs);
    prev_s0 = 0;
}
