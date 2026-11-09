// What ov_probe.c (--probe-services) lends ov_mkw_probe.c: calls to the overrides of its table by case key, on the
// guest main thread, and its check counters.
#pragma once
#include <stdint.h>

#include "host.h"

// The override with x0..x7 = a[0..7]; checks that callee-saved registers and sp survive. Returns x0.
uint64_t ovp_call(const char *key, const uint64_t a[8]);
#define OVP_CALL(key, ...) ovp_call(key, (const uint64_t[8]){__VA_ARGS__})
// The same with x8 (an indirect result address) and nstack arguments past the eighth on the guest stack.
uint64_t ovp_call_full(const char *key, const uint64_t a[8], uint64_t x8, const uint64_t *stack, int nstack);
void ovp_check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void *ovp_alloc(uint64_t n);                         // guest-visible memory
Cpu *ovp_cpu(void);

void probe_mkw_services(void);                       // ov_mkw_probe.c
