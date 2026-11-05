#pragma once

#include "sts_int8_model.h"

// M1a keeps the original double arithmetic order; M1b is a separately gated
// float32 candidate. Neither changes the fixed model or decision thresholds.
enum word23_accel_candidate_t { WORD23_ACCEL_M1A = 1, WORD23_ACCEL_M1B = 2 };
enum word23_accel_layer_t {
    WORD23_FRAMES_INPUT, WORD23_CONV1, WORD23_POOL1, WORD23_CONV2_INPUT,
    WORD23_CONV2, WORD23_POOL2, WORD23_CONV3_INPUT, WORD23_CONV3,
    WORD23_POOLED, WORD23_STATS_INPUT, WORD23_STATS, WORD23_MERGED,
    WORD23_HEAD1_INPUT, WORD23_HEAD1, WORD23_HEAD2_INPUT, WORD23_LOGITS,
    WORD23_ACCEL_LAYER_COUNT
};
struct word23_accel_telemetry_t {
    size_t workspace_bytes;
    uint32_t input_tensor_hash;
    float scales[WORD23_ACCEL_LAYER_COUNT];
    // Compute regions exclude quantization. Clock calls exist only when
    // WORD23_ACCEL_PROFILE=1; these fields are zero in performance builds.
    uint64_t preparation_us, conv1_us, pool1_us, conv2_us, pool2_us, conv3_us;
    uint64_t summary_us, stats_us, head1_us, head2_us, quantization_us, total_us;
};
using word23_accel_observer_t = void (*)(word23_accel_layer_t layer,
    const float *values, size_t count, float scale, void *user);

size_t word23_accel_workspace_bytes();
const char *word23_accel_layer_name(word23_accel_layer_t layer);
// Workspace must be 16-byte aligned and independent of feature/logits; one
// workspace belongs to one in-flight call. No allocation occurs in this API.
// Observer sees each quantize/dequantize point, before subsequent ReLU.
bool word23_accel_predict(const float *feature, size_t feature_count,
    float *logits, size_t logits_count, void *workspace, size_t workspace_bytes,
    word23_accel_candidate_t candidate, word23_accel_telemetry_t *telemetry = nullptr,
    word23_accel_observer_t observer = nullptr, void *observer_user = nullptr);

// When WORD23_ACCEL_REPLACE_BASELINE=1, this translation unit also implements
// the original sts_int8_predict... symbol with a persistent workspace. Do not
// link the original kernel in that build. WORD23_ACCEL_MODE selects 1 or 2.
// That convenience wrapper is single-task only; explicit workspace API is reentrant.
const word23_accel_telemetry_t *word23_accel_last_telemetry();

// Readonly-original copy with hooks, linked only in audit/profile builds. Its
// outputs must first be checked byte-for-byte against the original kernel.
bool word23_accel_baseline_predict(const float *feature, size_t feature_count,
    float *logits, size_t logits_count, word23_accel_telemetry_t *telemetry = nullptr,
    word23_accel_observer_t observer = nullptr, void *observer_user = nullptr);
