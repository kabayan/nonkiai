#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

namespace word23_accel_m2_detail {
constexpr int kMaxTerms = 200;
constexpr int kPaddedTerms = (kMaxTerms + 15) / 16 * 16;
// A digit may be -128 (unlike symmetric qint8 activations). Its product with
// a symmetric weight is bounded by 128*127, not 127*127.
constexpr int kDotBound = kMaxTerms * 128 * 127;
static_assert(kDotBound == 3251200 && kDotBound < INT32_MAX, "digit dot overflow");
struct alignas(16) Patch {
    int8_t planes[4][kPaddedTerms];
    int8_t weights[kPaddedTerms];
    int32_t zero_accumulator[4];
};
inline uint32_t float_bits(float value) {
    uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); return bits;
}
inline int scale_binary_exponent(float scale) {
    const uint32_t bits = float_bits(scale);
    // Quantization floor is 1e-8: every model scale is positive and normal.
    assert((bits >> 31) == 0 && ((bits >> 23) & 255) > 0 && ((bits >> 23) & 255) < 255);
    return static_cast<int>((bits >> 23) & 255) - 127 - 23;
}
// Exact signed integer representation of a finite dequantized float in units
// of 2^binary_exponent. This consumes the already rounded float32(q*scale),
// preserving that rounding rather than reconstructing q*scale in double.
inline bool float_units(float value, int binary_exponent, int32_t &units) {
    const uint32_t bits = float_bits(value), magnitude = bits & 0x7fffffff;
    if (!magnitude) { units = 0; return true; }
    const int exponent = static_cast<int>((bits >> 23) & 255);
    if (exponent == 0 || exponent == 255) return false;
    const int shift = exponent - 127 - 23 - binary_exponent;
    if (shift < 0 || shift > 7) return false;
    const uint64_t absolute = static_cast<uint64_t>((magnitude & 0x7fffff) | 0x800000) << shift;
    if (absolute > static_cast<uint64_t>(INT32_MAX)) return false;
    units = bits >> 31 ? -static_cast<int32_t>(absolute) : static_cast<int32_t>(absolute);
    return true;
}
inline bool balanced_digits(int32_t units, int8_t *digits) {
    int64_t remaining = units;
    for (int j = 0; j < 4; ++j) {
        const int byte = static_cast<int>(static_cast<uint64_t>(remaining) & 255);
        const int digit = byte >= 128 ? byte - 256 : byte;
        digits[j] = static_cast<int8_t>(digit);
        remaining = (remaining - digit) / 256;
    }
    return remaining == 0;
}
inline int64_t combine(const int32_t *dots) {
    return static_cast<int64_t>(dots[0]) + 256LL * dots[1] +
        65536LL * dots[2] + 16777216LL * dots[3];
}
#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)
extern "C" int32_t word23_dot_s8_s3(const int8_t *a, const int8_t *b,
    int padded_length, const int32_t *zero_accumulator);
#endif
inline int32_t dot(const int8_t *a, const int8_t *b, int padded_length,
    const int32_t *zero_accumulator) {
    assert(padded_length > 0 && padded_length <= kPaddedTerms && padded_length % 16 == 0);
    assert(reinterpret_cast<uintptr_t>(a) % 16 == 0 && reinterpret_cast<uintptr_t>(b) % 16 == 0);
#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)
    return word23_dot_s8_s3(a, b, padded_length, zero_accumulator);
#else
    (void)zero_accumulator;
    int32_t result = 0;
    for (int i = 0; i < padded_length; ++i)
        result += static_cast<int32_t>(a[i]) * static_cast<int32_t>(b[i]);
    return result;
#endif
}
}
