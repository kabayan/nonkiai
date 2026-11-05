#pragma once
#include "word23_accel_model.h"
enum word23_m2_candidate_t { WORD23_ACCEL_M2P = 3, WORD23_ACCEL_M2D = 4 };
size_t word23_m2_workspace_bytes();
// Same observer points as M1a/b; independent caller-owned 16-byte aligned arena.
// M2p accumulates symmetric qint8 and restores scales once; M2d first encodes
// existing float32 dequantized activations exactly into four signed digit planes.
// Both change the original double accumulation order and require retention gates.
bool word23_m2_predict(const float *feature, size_t feature_count, float *logits,
    size_t logits_count, void *workspace, size_t workspace_bytes,
    word23_m2_candidate_t candidate, word23_accel_telemetry_t *telemetry = nullptr,
    word23_accel_observer_t observer = nullptr, void *observer_user = nullptr);
extern "C" int word23_m2_dot_self_test();
