#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int notif_ima_adpcm_decode_block(
    int16_t *output, const uint8_t *input, size_t input_size, int channels);

#ifdef __cplusplus
}
#endif
