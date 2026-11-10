// input_script.c, for input.c: the script's presses ORed into one poll's buttons and sticks (sl, sr: x, y), and the
// merged poll written to MKW_INPUT_RECORD.
#pragma once
#include <stdint.h>

void script_apply(uint64_t *b, int32_t sl[2], int32_t sr[2], int player);   // player 0-7 (input_poll_player)
void rec_apply(uint64_t b, const int32_t sl[2], const int32_t sr[2]);
