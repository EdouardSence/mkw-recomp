// What vk_usnap.c shares with vk_usnap_take.c: the uniform ranges a submission binds, collected and sorted.
#ifndef VK_USNAP_INT_H
#define VK_USNAP_INT_H
#include <stdint.h>

#include "nvn_api.h"

#define ALIGN 256u
#define MAX_RANGE (64u << 10)
#define CALL_DEPTH 16

typedef struct { uint64_t lo, hi; } Rng;
typedef struct { Rng *r; int n, cap; } RngVec;

void usnap_collect(RngVec *v, const op_hdr *h, int depth);   // every OP_BIND_UBO range, aligned out to ALIGN
void usnap_sort(RngVec *v);                                  // by lo

// MKW_USNAP_AT_SUBMIT (vk_usnap_take.c): the bytes of a submission's uniform ranges as the submit found them.
typedef struct UsnapTake UsnapTake;
UsnapTake *usnap_take_pop(const uint64_t *handles, int n);   // the replay thread, in submit order; NULL none
const uint8_t *usnap_take_bytes(const UsnapTake *t, uint64_t lo, uint64_t hi);   // NULL: not taken
void usnap_take_free(UsnapTake *t);
int usnap_take_compare(void);   // MKW_USNAP_CMP: taken only to count what changed before the recording
#endif
