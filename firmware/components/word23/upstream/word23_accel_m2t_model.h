#pragma once
#include "word23_accel_m2_model.h"
// M2t is separately versioned and only accepts the exact-dequant M2D arithmetic.
// Caller arena/observer/profile contracts are identical to the original M2 API.
size_t word23_m2t_workspace_bytes();
bool word23_m2t_predict(const float *feature, size_t feature_count, float *logits,
    size_t logits_count, void *workspace, size_t workspace_bytes,
    word23_m2_candidate_t candidate = WORD23_ACCEL_M2D,
    word23_accel_telemetry_t *telemetry = nullptr,
    word23_accel_observer_t observer = nullptr, void *observer_user = nullptr);
