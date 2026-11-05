#pragma once

#include <stdint.h>

// Direction IDs match the recognized intents, but manual sending is independent.
static inline uint32_t nk_ir_saved_value(uint8_t direction)
{
    const uint32_t values[] = {0x00FF807F, 0x00FF20DF, 0x00FF08F7, 0x00FF40BF};
    return direction < 4 ? values[direction] : 0;
}

// DLS-147: normal sends repeat full frames; forward/backward need one more.
static inline uint8_t nk_ir_default_repeat(uint8_t direction)
{
    const uint8_t counts[] = {5, 5, 4, 4};
    return direction < 4 ? counts[direction] : 0;
}

static inline uint16_t nk_ir_bit_space(uint32_t value, unsigned bit)
{
    return (value & (UINT32_C(1) << (31 - bit))) ? 1680 : 560;
}
