// Independent M2t candidate: temporal-patch reuse. Frozen M2/M1 source stays unchanged.
#include "word23_accel_m2t_model.h"
#include "word23_accel_m2_detail.h"
#include "word23_accel_internal.h"
#include "sts_int8_model_data.h"

#include <cstring>
#include <new>
#if WORD23_ACCEL_PROFILE
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <chrono>
#endif
#endif

namespace {
template<typename T, size_t N> constexpr size_t countof(const T (&)[N]) { return N; }
constexpr int C1 = countof(STS_G5_FRAMES_0_BIAS_FP32);
constexpr int C2 = countof(STS_G5_FRAMES_4_BIAS_FP32);
constexpr int H = countof(STS_G5_HEAD_0_BIAS_FP32);
constexpr int F = STS_G5_FEATURE_FRAMES, M = STS_G5_MEL_BINS, S = STS_G5_STATS_DIM;
constexpr int P = 2 * C2, MERGED = P + C1;
constexpr int A = std::max(std::max(M * F, C1 * F / 2), std::max(C2 * F / 4, MERGED));
constexpr int B = std::max(std::max(C1 * F, C2 * F / 2), std::max(std::max(C2 * F / 4, S), H));
struct alignas(16) Workspace { float a[A]; float b[B]; int8_t input_planes[4][A]; word23_accel_m2_detail::Patch patch; };
static_assert(F % 4 == 0 && STS_G5_FEATURE_DIM == M * F + S, "feature shape");
static_assert(C1 == 32 && C2 == 64 && H == 64 && STS_G5_CLASS_COUNT == 10, "fixed v0.4 dimensions required");
static_assert(countof(STS_G5_FRAMES_0_WEIGHT_QINT8) == C1 * M * 5, "conv1 shape");
static_assert(countof(STS_G5_FRAMES_4_WEIGHT_QINT8) == C2 * C1 * 5, "conv2 shape");
static_assert(countof(STS_G5_FRAMES_8_WEIGHT_QINT8) == C2 * C2 * 3, "conv3 shape");
static_assert(countof(STS_G5_STATS_0_WEIGHT_QINT8) == C1 * S, "stats shape");
static_assert(countof(STS_G5_HEAD_0_WEIGHT_QINT8) == H * MERGED, "head1 shape");
static_assert(countof(STS_G5_HEAD_3_WEIGHT_QINT8) == STS_G5_CLASS_COUNT * H, "head2 shape");
static_assert(countof(STS_G5_FRAMES_0_WEIGHT_SCALE) == C1 && countof(STS_G5_FRAMES_4_WEIGHT_SCALE) == C2 &&
    countof(STS_G5_FRAMES_8_WEIGHT_SCALE) == C2 && countof(STS_G5_STATS_0_WEIGHT_SCALE) == C1 &&
    countof(STS_G5_HEAD_0_WEIGHT_SCALE) == H && countof(STS_G5_HEAD_3_WEIGHT_SCALE) == STS_G5_CLASS_COUNT, "scale shape");
static_assert(countof(STS_G5_FRAMES_8_BIAS_FP32) == C2 && countof(STS_G5_STATS_0_BIAS_FP32) == C1 &&
    countof(STS_G5_HEAD_3_BIAS_FP32) == STS_G5_CLASS_COUNT, "bias shape");

inline uint64_t now_us() {
#if WORD23_ACCEL_PROFILE
#ifdef ESP_PLATFORM
    return static_cast<uint64_t>(esp_timer_get_time());
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
#else
    return 0;
#endif
}
struct Context {
    word23_accel_telemetry_t &telemetry;
    word23_accel_observer_t observer;
    void *user;
    void quant(float *values, size_t count, word23_accel_layer_t layer) {
        const uint64_t start = now_us();
        const float scale = word23_accel_detail::symmetric_scale(values, count);
        for (size_t i = 0; i < count; ++i)
            values[i] = static_cast<float>(word23_accel_detail::quantize_value(values[i], scale)) * scale;
        telemetry.quantization_us += now_us() - start;
        telemetry.scales[layer] = scale;
        if (observer) observer(layer, values, count, scale, user);
    }
};

bool prepare_planes(const float *input, size_t count, float input_scale,
    Workspace &w, word23_m2_candidate_t candidate) {
    const int exponent = word23_accel_m2_detail::scale_binary_exponent(input_scale);
    for (size_t i=0;i<count;++i) {
        if (candidate == WORD23_ACCEL_M2P)
            w.input_planes[0][i] = word23_accel_detail::quantize_value(input[i], input_scale);
        else {
            int32_t units; int8_t digits[4];
            if (!word23_accel_m2_detail::float_units(input[i], exponent, units) ||
                !word23_accel_m2_detail::balanced_digits(units, digits)) return false;
            for (int p=0;p<4;++p) w.input_planes[p][i] = digits[p];
        }
    }
    // Every SIMD reduction explicitly clears ACCX from this all-zero buffer.
    std::memset(&w.patch, 0, sizeof(w.patch));
    return true;
}
void m2_conv(const float *input, int in_channels, int length, const int8_t *weight,
    const float *scale, const float *bias, int out_channels, int kernel,
    float *output, float input_scale, Workspace &w, word23_m2_candidate_t candidate) {
    const bool valid = prepare_planes(input, static_cast<size_t>(in_channels*length), input_scale, w, candidate);
    assert(valid); (void)valid;
    const int terms=in_channels*kernel, padded=(terms+15)/16*16;
    const int planes=candidate==WORD23_ACCEL_M2D ? 4 : 1;
    const int exponent=word23_accel_m2_detail::scale_binary_exponent(input_scale);
    // Version M2t: prepare each temporal patch once, then reuse all four digit
    // planes for every output channel. Only one small aligned weight tile lives
    // in the caller arena. Restore values preserve the M2d double expression.
    double restore_by_channel[C2];
    assert(out_channels<=C2);
    for(int oc=0;oc<out_channels;++oc)
        restore_by_channel[oc]=std::ldexp(static_cast<double>(scale[oc]),exponent);
    for (int t=0;t<length;++t) {
        for (int ic=0;ic<in_channels;++ic) {
            for (int k=0;k<kernel;++k) {
                const int src_t=t+k-kernel/2;
                for (int p=0;p<planes;++p)
                    w.patch.planes[p][ic*kernel+k]=src_t<0 || src_t>=length ? 0 : w.input_planes[p][ic*length+src_t];
            }
        }
        for(int oc=0;oc<out_channels;++oc) {
            std::memcpy(w.patch.weights,weight+oc*terms,terms);
            int32_t dots[4]{};
            for (int p=0;p<planes;++p)
                dots[p]=word23_accel_m2_detail::dot(w.patch.planes[p],w.patch.weights,padded,w.patch.zero_accumulator);
            const int64_t sum=word23_accel_m2_detail::combine(dots);
            output[oc*length+t]=static_cast<float>(static_cast<double>(bias[oc])+static_cast<double>(sum)*restore_by_channel[oc]);
        }
    }
}
void m2_linear(const float *input, const int8_t *weight, const float *scale,
    const float *bias, int out_features, int in_features, float *output,
    float input_scale, Workspace &w, word23_m2_candidate_t candidate) {
    const bool valid=prepare_planes(input, static_cast<size_t>(in_features), input_scale, w, candidate);
    assert(valid); (void)valid;
    const int padded=(in_features+15)/16*16;
    const int planes=candidate==WORD23_ACCEL_M2D ? 4 : 1;
    const int exponent=word23_accel_m2_detail::scale_binary_exponent(input_scale);
    for (int p=0;p<planes;++p) std::memcpy(w.patch.planes[p],w.input_planes[p],in_features);
    for (int o=0;o<out_features;++o) {
        std::memcpy(w.patch.weights,weight+o*in_features,in_features);
        int32_t dots[4]{};
        for (int p=0;p<planes;++p)
            dots[p]=word23_accel_m2_detail::dot(w.patch.planes[p],w.patch.weights,padded,w.patch.zero_accumulator);
        const int64_t sum=planes==4 ? word23_accel_m2_detail::combine(dots) : dots[0];
        const double restore=candidate==WORD23_ACCEL_M2D ?
            std::ldexp(static_cast<double>(scale[o]),exponent) :
            static_cast<double>(input_scale)*static_cast<double>(scale[o]);
        output[o]=static_cast<float>(static_cast<double>(bias[o])+static_cast<double>(sum)*restore);
    }
}
void predict(const float *feature, float *logits, Workspace &w, Context &ctx, word23_m2_candidate_t candidate) {
    using namespace word23_accel_detail;
    auto &p = ctx.telemetry;
    uint64_t start = now_us();
    std::memcpy(w.a, feature, M * F * sizeof(float));
    p.preparation_us += now_us() - start;
    ctx.quant(w.a, M * F, WORD23_FRAMES_INPUT);
    start = now_us();
    m2_conv(w.a, M, F, STS_G5_FRAMES_0_WEIGHT_QINT8, STS_G5_FRAMES_0_WEIGHT_SCALE, STS_G5_FRAMES_0_BIAS_FP32, C1, 5, w.b, p.scales[WORD23_FRAMES_INPUT], w, candidate);
    p.conv1_us += now_us() - start;
    ctx.quant(w.b, C1 * F, WORD23_CONV1);
    relu(w.b, C1 * F);
    start = now_us();
    max_pool(w.b, C1, F, w.a);
    p.pool1_us += now_us() - start;
    ctx.quant(w.a, C1 * F / 2, WORD23_POOL1);
    // Do not eliminate these consecutive quantization points: recomputing a
    // scale from float32 dequantized values can change a half-way rounding.
    ctx.quant(w.a, C1 * F / 2, WORD23_CONV2_INPUT);
    start = now_us();
    m2_conv(w.a, C1, F / 2, STS_G5_FRAMES_4_WEIGHT_QINT8, STS_G5_FRAMES_4_WEIGHT_SCALE, STS_G5_FRAMES_4_BIAS_FP32, C2, 5, w.b, p.scales[WORD23_CONV2_INPUT], w, candidate);
    p.conv2_us += now_us() - start;
    ctx.quant(w.b, C2 * F / 2, WORD23_CONV2);
    relu(w.b, C2 * F / 2);
    start = now_us();
    max_pool(w.b, C2, F / 2, w.a);
    p.pool2_us += now_us() - start;
    ctx.quant(w.a, C2 * F / 4, WORD23_POOL2);
    ctx.quant(w.a, C2 * F / 4, WORD23_CONV3_INPUT);
    start = now_us();
    m2_conv(w.a, C2, F / 4, STS_G5_FRAMES_8_WEIGHT_QINT8, STS_G5_FRAMES_8_WEIGHT_SCALE, STS_G5_FRAMES_8_BIAS_FP32, C2, 3, w.b, p.scales[WORD23_CONV3_INPUT], w, candidate);
    p.conv3_us += now_us() - start;
    ctx.quant(w.b, C2 * F / 4, WORD23_CONV3);
    relu(w.b, C2 * F / 4);
    start = now_us();
    for (int c = 0; c < C2; ++c) {
        float sum = 0.0f, maximum = w.b[c * (F / 4)];
        for (int t = 0; t < F / 4; ++t) {
            const float value = w.b[c * (F / 4) + t];
            sum += value;
            maximum = std::max(maximum, value);
        }
        w.a[c] = sum / static_cast<float>(F / 4);
        w.a[C2 + c] = maximum;
    }
    p.summary_us += now_us() - start;
    ctx.quant(w.a, P, WORD23_POOLED);
    start = now_us();
    std::memcpy(w.b, feature + M * F, S * sizeof(float));
    p.preparation_us += now_us() - start;
    ctx.quant(w.b, S, WORD23_STATS_INPUT);
    start = now_us();
    m2_linear(w.b, STS_G5_STATS_0_WEIGHT_QINT8, STS_G5_STATS_0_WEIGHT_SCALE, STS_G5_STATS_0_BIAS_FP32, C1, S, w.a + P, p.scales[WORD23_STATS_INPUT], w, candidate);
    p.stats_us += now_us() - start;
    ctx.quant(w.a + P, C1, WORD23_STATS);
    relu(w.a + P, C1);
    ctx.quant(w.a, MERGED, WORD23_MERGED);
    ctx.quant(w.a, MERGED, WORD23_HEAD1_INPUT);
    start = now_us();
    m2_linear(w.a, STS_G5_HEAD_0_WEIGHT_QINT8, STS_G5_HEAD_0_WEIGHT_SCALE, STS_G5_HEAD_0_BIAS_FP32, H, MERGED, w.b, p.scales[WORD23_HEAD1_INPUT], w, candidate);
    p.head1_us += now_us() - start;
    ctx.quant(w.b, H, WORD23_HEAD1);
    relu(w.b, H);
    ctx.quant(w.b, H, WORD23_HEAD2_INPUT);
    start = now_us();
    m2_linear(w.b, STS_G5_HEAD_3_WEIGHT_QINT8, STS_G5_HEAD_3_WEIGHT_SCALE, STS_G5_HEAD_3_BIAS_FP32, STS_G5_CLASS_COUNT, H, w.a, p.scales[WORD23_HEAD2_INPUT], w, candidate);
    p.head2_us += now_us() - start;
    ctx.quant(w.a, STS_G5_CLASS_COUNT, WORD23_LOGITS);
    std::memcpy(logits, w.a, STS_G5_CLASS_COUNT * sizeof(float));
}
}

size_t word23_m2t_workspace_bytes() { return sizeof(Workspace); }
bool word23_m2t_predict(const float *feature, size_t feature_count, float *logits,
    size_t logits_count, void *workspace, size_t workspace_bytes,
    word23_m2_candidate_t candidate, word23_accel_telemetry_t *telemetry,
    word23_accel_observer_t observer, void *observer_user) {
    if (!feature || !logits || !workspace || feature_count != STS_G5_FEATURE_DIM ||
        logits_count < STS_G5_CLASS_COUNT || workspace_bytes < sizeof(Workspace) ||
        reinterpret_cast<uintptr_t>(workspace) % alignof(Workspace) != 0 ||
        candidate != WORD23_ACCEL_M2D) return false;
    word23_accel_telemetry_t local{};
    auto &p = telemetry ? *telemetry : local;
    p = {};
    p.workspace_bytes = sizeof(Workspace);
    const uint64_t start = now_us();
    // Match baseline telemetry without spending a tensor hash when not requested.
    if (telemetry) p.input_tensor_hash = word23_accel_detail::tensor_hash(feature, feature_count);
    auto *w = ::new (workspace) Workspace;
    Context ctx{p, observer, observer_user};
    predict(feature, logits, *w, ctx, candidate);
    p.total_us = now_us() - start;
    return true;
}
