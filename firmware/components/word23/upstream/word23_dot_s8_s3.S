// Independent ESP32-S3 signed int8 SIMD reduction. Both arrays are 16-byte
// aligned; caller zero-pads the tail and supplies a positive multiple of 16.
// At most 208 entries, so ACCX fits int32 even for balanced digit -128.
// The zero accumulator buffer is aligned and contains four zero int32 words.
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#ifdef CONFIG_IDF_TARGET_ESP32S3
    .text
    .align 4
    .global word23_dot_s8_s3
    .type word23_dot_s8_s3,@function
word23_dot_s8_s3:
    entry a1, 16
    ee.ld.accx.ip a5, 0
    srli a4, a4, 4
    loopnez a4, .Lword23_dot_done
        ee.vld.128.ip q0, a2, 16
        ee.vld.128.ip q1, a3, 16
        ee.vmulas.s8.accx q0, q1
.Lword23_dot_done:
    rur.accx_0 a2
    retw.n
    .size word23_dot_s8_s3, .-word23_dot_s8_s3
#endif
