/*
 * Decoder-only adaptation of ADPCM-XQ by David Bryant.
 * Copyright (c) David Bryant. All rights reserved.
 * Distributed under the 3-clause BSD license in LICENSE.adpcm-xq.
 */

#include "ima_adpcm_decoder.h"

static const uint16_t kStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static const int8_t kIndexTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static int clamp(int value, int minimum, int maximum) {
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int16_t decode_nibble(uint8_t nibble, int32_t *pcm, int *index) {
    const uint16_t step = kStepTable[*index];
    int32_t delta = step >> 3;
    if (nibble & 1) delta += step >> 2;
    if (nibble & 2) delta += step >> 1;
    if (nibble & 4) delta += step;
    *pcm += (nibble & 8) ? -delta : delta;
    *pcm = clamp(*pcm, -32768, 32767);
    *index = clamp(*index + kIndexTable[nibble & 7], 0, 88);
    return (int16_t)*pcm;
}

int notif_ima_adpcm_decode_block(
    int16_t *output, const uint8_t *input, size_t input_size, int channels) {
    if (output == NULL || input == NULL || channels < 1 || channels > 2 ||
        input_size < (size_t)channels * 4) {
        return 0;
    }

    int32_t pcm[2] = {0};
    int index[2] = {0};
    for (int channel = 0; channel < channels; ++channel) {
        pcm[channel] = (int16_t)(input[0] | (input[1] << 8));
        index[channel] = input[2];
        if (index[channel] > 88 || input[3] != 0) return 0;
        output[channel] = (int16_t)pcm[channel];
        input += 4;
        input_size -= 4;
    }

    const size_t chunks = input_size / ((size_t)channels * 4);
    size_t sample = 1;
    for (size_t chunk = 0; chunk < chunks; ++chunk) {
        for (int channel = 0; channel < channels; ++channel) {
            for (int byte = 0; byte < 4; ++byte) {
                const uint8_t packed = *input++;
                output[((sample + (size_t)byte * 2) * channels) + channel] =
                    decode_nibble(packed & 0x0f, &pcm[channel], &index[channel]);
                output[((sample + (size_t)byte * 2 + 1) * channels) + channel] =
                    decode_nibble(packed >> 4, &pcm[channel], &index[channel]);
            }
        }
        sample += 8;
    }
    return (int)sample;
}
