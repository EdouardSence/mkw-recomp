// MKW_REC_DIFF=1 (vk_rec_diff.c): how much of each submission's resolved Vulkan commands repeats the previous frame's.
#pragma once
#include <stdint.h>

extern int rec_diff_on;   // -1 not read yet
// One packet of the recorder's ring: p its words after the header, lo-hi the ring (pointers there are made relative),
// loose_zero the sorted word indexes hashed as zero in the loose hash (uniform block offsets).
void rec_diff_packet(uint32_t op, const uint64_t *p, uint32_t words, int is_draw, const uint64_t *lo, const uint64_t *hi,
                     const uint32_t *loose_zero, int nloose);
void rec_diff_begin(void);   // a submission's recording starts
void rec_diff_end(void);   // and ends: matched and logged
const char *rec_op_name(uint32_t op);   // vk_rec.c: a packet's command
