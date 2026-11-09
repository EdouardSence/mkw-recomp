// MKW_REUSE: the recorder ring's side (vk_rec_reuse.c) of reusing a draw's packets in a later frame (vk_exec_reuse.c).
#pragma once
#include <stdint.h>

#include "vk.h"

#define REC_SLOTS 128
#define REC_WIN_WORDS 384
#define REC_WIN_PACKETS 48
#define REC_WIN_RELOCS 64
#define REC_WIN_PAIRS 128

// One draw's packets as captured (descriptor-set binds left out), with the bound-state slots before them and the
// slots they set.
typedef struct {
    uint64_t before[REC_SLOTS];
    uint64_t written[REC_SLOTS / 64];
    uint32_t nwords, nreloc, npk;
    uint16_t reloc[REC_WIN_RELOCS];   // words holding a pointer into their packet, kept as the offset from its payload
    uint64_t words[REC_WIN_WORDS];    // packets: header word, payload
    // The bound-state slots each packet sets, with their values: pairs [pk_pair[k], pk_pair[k + 1]) of packet k. At
    // emission a packet whose slots all hold its values already is left out.
    uint32_t npair;
    uint16_t pk_pair[REC_WIN_PACKETS + 1];
    uint8_t psl[REC_WIN_PAIRS];
    uint64_t ph[REC_WIN_PAIRS];
} RecWin;

extern int rec_slots_on;                                     // set by vk_exec_reuse.c: the slots follow every packet
int rec_owned(VkCommandBuffer cb);                           // cb is recorded through the ring now
void rec_slots_reset(void);                                  // a part's command buffer begins
void rec_slot_note(uint32_t op, const uint64_t *p, uint32_t words);   // from rec_pub: a published packet
void rec_cap_note_fallback(void);                            // a call made without a packet
int rec_cap_begin(VkCommandBuffer cb, RecWin *w);            // 0: cb is not recorded through the ring
int rec_cap_end(void);                                       // 1: w holds a draw that can be reused
int rec_cap_why(void);   // why rec_cap_end refused: 0 a fallback or new part, 62 no draw last, 63 too large, else the op
const char *rec_op_name(uint32_t op);
int rec_slots_match(const uint8_t *sl, const uint64_t *h, int n);
void rec_win_emit(VkCommandBuffer cb, const RecWin *w, int from, int to);   // packets [from, to) of w

// vk_rec.c: the producer's position and the ring word at a position.
uint64_t rec_put_pos(void);
const uint64_t *rec_ring_word(uint64_t pos);
