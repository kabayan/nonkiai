#pragma once
#include "log_mel_feature.h"
#ifdef __cplusplus
extern "C" {
#endif
int sts_f2_log_mel_feature(const float *pcm, size_t sample_count, float *feature_out);
int sts_f2_log_mel_feature_i16(const short *pcm, size_t sample_count, float *feature_out);
// Optional allocation-free feature API. Workspace is 16-byte aligned, separate
// from PCM/feature_out, and can be reused as the M2 arena after this call returns.
// Returns 1 for short PCM, 5 for null/misaligned/undersized workspace.
size_t sts_f2_workspace_bytes(size_t sample_count);
int sts_f2_log_mel_feature_i16_with_workspace(const short *pcm, size_t sample_count,
    float *feature_out, void *workspace, size_t workspace_bytes);
int sts_f2_log_mel_feature_with_workspace(const float *pcm, size_t sample_count,
    float *feature_out, void *workspace, size_t workspace_bytes);
void sts_f2_normalize_feature(const float *feature, const float *mean, const float *std, float *out);
size_t sts_f2_mel_cache_bytes(); // Includes filter edges and fixed sparse capacity.
size_t sts_f2_mel_nonzero_weights();
size_t sts_f2_feature_static_bytes(); // Window + FFT tables + sparse filterbank.
#ifdef __cplusplus
}
#endif
