#pragma once

#include <stddef.h>

/*
 * On-device log-mel feature extraction.
 *
 * Numerically mirrors training/train_fp32.py `log_mel_feature_from_pcm` with the
 * resample step skipped: the caller supplies PCM that is ALREADY 16 kHz mono
 * (on ESP32 the ES8311 codec is configured for 16 kHz, so no resample happens
 * on-device; the resample only exists in the PC dataset pipeline because the TTS
 * WAVs are 48 kHz). The G6a parity gate feeds identical 16 kHz int16 PCM to this
 * C++ path and to the Python reference so that only the extraction is compared.
 *
 * Pipeline: STFT(hann-periodic, nperseg=400, noverlap=240, nfft=512,
 * scaling='spectrum') -> power -> mel(40x257) -> log(max(.,1e-8)) -> per-bin
 * time normalization -> linear interp to 64 frames (2560) -> per-bin
 * mean/std/min/max stats (160) -> concat = 2720 (pre global normalization).
 */

#define STS_FEAT_SAMPLE_RATE 16000
#define STS_FEAT_FRAME_LENGTH 400 /* 25 ms @ 16 kHz */
#define STS_FEAT_FRAME_SHIFT 160  /* 10 ms @ 16 kHz */
#define STS_FEAT_NFFT 512
#define STS_FEAT_MEL_BINS 40
#define STS_FEAT_FRAMES 64
#define STS_FEAT_STATS_DIM 160
#define STS_FEAT_DIM 2720 /* 40*64 + 160 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compute the raw (pre global-normalization) 2720-d log-mel feature from 16 kHz
 * mono PCM in [-1, 1). `feature_out` must hold STS_FEAT_DIM floats.
 * Returns 0 on success, 1 if sample_count < STS_FEAT_FRAME_LENGTH.
 */
int sts_log_mel_feature(const float *pcm, size_t sample_count, float *feature_out);

/* Equivalent input path for signed 16-bit PCM; avoids an utterance-sized float
 * copy on RAM-constrained devices. */
int sts_log_mel_feature_i16(const short *pcm, size_t sample_count, float *feature_out);

/*
 * Apply global normalization in-place-safe: out[i] = (feature[i] - mean[i]) / std[i].
 * mean/std are the per-dimension training statistics (STS_FEAT_DIM each), matching
 * the student payload `feature_mean` / `feature_std`.
 */
void sts_normalize_feature(const float *feature, const float *mean, const float *std, float *out);

#ifdef __cplusplus
}
#endif
