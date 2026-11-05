#pragma once

#include <stddef.h>
#include <stdint.h>

#define STS_G5_MEL_BINS 40
#define STS_G5_FEATURE_FRAMES 64
#define STS_G5_STATS_DIM 160
#define STS_G5_FEATURE_DIM 2720
#if defined(__has_include)
#if __has_include("sts_int8_model_config.h")
#include "sts_int8_model_config.h"
#endif
#endif
#ifndef STS_G5_CLASS_COUNT
#error "sts_int8_model_config.h must define STS_G5_CLASS_COUNT"
#endif

typedef struct {
    int input_tensor_match;
    uint32_t input_tensor_hash;
    size_t arena_bytes_estimate;
} sts_int8_telemetry_t;

void sts_int8_predict_from_normalized_feature(
    const float *feature,
    size_t feature_count,
    float *logits,
    size_t logits_count,
    sts_int8_telemetry_t *telemetry);
