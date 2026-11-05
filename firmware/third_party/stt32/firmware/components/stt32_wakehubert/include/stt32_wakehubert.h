#pragma once

/*
 * 17-intent recognition core (DLS-242/243): wakehubert (int8) + 10 classifiers.
 * One 3 s window of 16 kHz int16 PCM -> trim_bounds -> log-mel -> int8 network ->
 * mean of the 10 softmax vectors. Ported from the verified AtomS3 firmware
 * (feature/word-stt-23, atoms3_wakehubert_padfix_fwtest_20261025/main/main.cpp).
 *
 * Every work buffer and table is allocated once in init() with heap_caps
 * (16-byte aligned, cfg->work_caps). infer() never allocates.
 */

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "stt32_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Class count of the weights. The exporter writes it as STT32_WH_NC into generated/qlayout.h; CMakeLists.txt and
 * host/build_host.sh read it from there and pass -DSTT32_WH_CLASSES=<n>. wakehubert.cpp static_asserts the two agree. */
#ifndef STT32_WH_CLASSES
#define STT32_WH_CLASSES 17
#endif
#define STT32_WH_NETS 10

typedef struct {
    int seg_s, seg_e;   /* trimmed range inside the window, in samples */
    int T, F;           /* log-mel frames, encoder frames */
    uint32_t mel_crc;   /* FNV-1a of the int8 mel (T x 64), as the old firmware printed */
    uint32_t feat_crc;  /* FNV-1a of the int8 encoder output (F x 128) */
    int32_t acc[STT32_WH_NETS][STT32_WH_CLASSES]; /* integer fc results per classifier */
} stt32_wakehubert_info_t;

esp_err_t stt32_wakehubert_init(const stt32_backend_config_t *cfg);
void stt32_wakehubert_deinit(void);
/* Same contract as stt32_backend_t.infer; info may be NULL. */
int stt32_wakehubert_infer(const int16_t *pcm, size_t n, float *scores, size_t class_count,
                           stt32_wakehubert_info_t *info);

/* Frequency-response correction (DLS-1069 S-A) before trim. Compiled in with CONFIG_STT32_WAKEHUBERT_EQ
 * (host: -DSTT32_WH_EQ=1); then on by default. Returns -1 when asked to turn it on in a build without it. */
int stt32_wakehubert_set_eq(int on);
/* Test hooks (only in a build with the correction; init must have run): the correction alone, in -> out
 * (distinct buffers), n samples. eq_apply = the selected implementation, eq_apply_ref = the direct float FIR.
 * eq_stats (only with the int16 FIR): chunks / float-fallback chunks of the last eq_apply. */
void stt32_wakehubert_eq_apply(const int16_t *in, int16_t *out, int n);
/* Trim the uncorrected window, then correct only [*s, *e) (use_ref: float FIR, else the selected implementation);
 * out gets *e - *s samples. Returns 1 when there is no speech. */
int stt32_wakehubert_eq_seg(const int16_t *in, int n, int16_t *out, int use_ref, int *s, int *e);
void stt32_wakehubert_eq_apply_ref(const int16_t *in, int16_t *out, int n);
void stt32_wakehubert_eq_stats(long *chunks, long *fallback);

/* Allocation table for reports / tests: i = 0.. until false. */
int stt32_wakehubert_alloc_info(int i, const char **name, size_t *bytes);

#ifdef __cplusplus
}
#endif
