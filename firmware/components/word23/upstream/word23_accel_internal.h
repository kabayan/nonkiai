#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace word23_accel_detail {
inline float symmetric_scale(const float *values, size_t count) {
    float max_abs = 0.0f;
    for (size_t i = 0; i < count; ++i) max_abs = std::max(max_abs, std::fabs(values[i]));
    const float scale = max_abs / 127.0f;
    return scale > 1.0e-8f ? scale : 1.0e-8f;
}
// Keep lrint and the original float division. FE_TONEAREST is the existing
// runtime contract; substituting lround changes half-way values.
inline int8_t quantize_value(float value, float scale) {
    long rounded = std::lrint(value / scale);
    if (rounded > 127) rounded = 127;
    else if (rounded < -127) rounded = -127;
    return static_cast<int8_t>(rounded);
}
inline uint32_t tensor_hash(const float *values, size_t count) {
    const float scale = symmetric_scale(values, count);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; ++i) {
        hash ^= static_cast<uint8_t>(quantize_value(values[i], scale));
        hash *= 16777619u;
    }
    return hash;
}
inline void relu(float *values, size_t count) {
    for (size_t i = 0; i < count; ++i) if (values[i] < 0.0f) values[i] = 0.0f;
}
template<typename Acc>
inline void conv(const float *input, int in_channels, int length,
    const int8_t *weight, const float *scale, const float *bias,
    int out_channels, int kernel, float *output) {
    const int pad = kernel / 2;
    for (int oc = 0; oc < out_channels; ++oc) {
        for (int t = 0; t < length; ++t) {
            Acc acc = static_cast<Acc>(bias[oc]);
            for (int ic = 0; ic < in_channels; ++ic) {
                for (int k = 0; k < kernel; ++k) {
                    const int src_t = t + k - pad;
                    if (src_t < 0 || src_t >= length) continue;
                    const size_t wi = static_cast<size_t>((oc * in_channels + ic) * kernel + k);
                    acc += static_cast<Acc>(input[ic * length + src_t]) *
                        (static_cast<Acc>(weight[wi]) * static_cast<Acc>(scale[oc]));
                }
            }
            output[oc * length + t] = static_cast<float>(acc);
        }
    }
}
template<typename Acc>
inline void linear(const float *input, const int8_t *weight, const float *scale,
    const float *bias, int out_features, int in_features, float *output) {
    for (int o = 0; o < out_features; ++o) {
        Acc acc = static_cast<Acc>(bias[o]);
        for (int i = 0; i < in_features; ++i)
            acc += static_cast<Acc>(input[i]) *
                (static_cast<Acc>(weight[o * in_features + i]) * static_cast<Acc>(scale[o]));
        output[o] = static_cast<float>(acc);
    }
}
inline void max_pool(const float *input, int channels, int length, float *output) {
    for (int c = 0; c < channels; ++c)
        for (int t = 0; t < length / 2; ++t)
            output[c * (length / 2) + t] = std::max(input[c * length + 2 * t], input[c * length + 2 * t + 1]);
}
}
