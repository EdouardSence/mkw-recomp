// C entry points to bc7enc.cpp for vk_texpre.c: the settings of bc7enc_rdo's command line without RDO and with bc7enc.cpp
// (-C): every mode bc7enc.cpp has (1, 5, 6, 7), all partitions, highest uber level, linear weights.
#include "bc7enc.h"

static bc7enc_compress_block_params params;

extern "C" void bc7enc_c_init(void) {
    bc7enc_compress_block_init();
    bc7enc_compress_block_params_init(&params);
    bc7enc_compress_block_params_init_linear_weights(&params);
    params.m_uber_level = BC7ENC_MAX_UBER_LEVEL;
}

extern "C" void bc7enc_c_block(void *out16, const void *rgba64) { bc7enc_compress_block(out16, rgba64, &params); }
