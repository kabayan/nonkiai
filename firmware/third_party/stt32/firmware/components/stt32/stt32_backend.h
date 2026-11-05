#pragma once

/*
 * Internal seam between the stt32 lifecycle (stt32_service.c) and the
 * recognition core (DLS-242/243). The service owns capture: it cuts a fixed
 * 3 s window of 16 kHz int16 PCM (the same shape the 17-intent model was
 * evaluated on) and hands it to infer(). The device build links the
 * wakehubert backend (components/stt32_wakehubert). The host lifecycle test
 * supplies a fake through stt32_backend_default().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "stt32.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Samples in one inference window (3 s at 16 kHz). */
#define STT32_WINDOW_SAMPLES 48000

typedef struct {
    uint32_t work_caps;            /* heap_caps flags for work buffers and tables */
    int worker_core;               /* core of the second compute task */
    int worker_priority;
    /* Core the caller of infer() is pinned to; any value outside the device's core range
     * (e.g. tskNO_AFFINITY) means "not pinned". The backend splits work over both cores
     * with a spin barrier only when this task and the worker are pinned to different cores. */
    int infer_core;
    const char *weights_partition; /* NULL: weights linked into .rodata */
    /* NONE: read flash directly. GRU: copy the GRU weights to work_caps. ALL: copy the whole
     * blob. ESP_ERR_NO_MEM if the copy fails. (DEFAULT is treated as GRU.) */
    stt32_weights_copy_t weights_copy;
} stt32_backend_config_t;

typedef struct {
    /* Allocate every work buffer once (16-byte aligned, cfg->work_caps). */
    esp_err_t (*init)(const stt32_backend_config_t *cfg);
    void (*deinit)(void);
    /* Model output size and model index -> stable semantic id (valid after init). */
    size_t (*class_count)(void);
    const stt32_semantic_id_t *(*semantic_ids)(void);
    /* Averaged softmax scores for one window of int16 PCM.
     * 0 = ok, 1 = no speech found in the window (scores untouched), other = error. */
    int (*infer)(const int16_t *pcm, size_t sample_count, float *scores_out, size_t class_count);
} stt32_backend_t;

const stt32_backend_t *stt32_backend_default(void);

#ifdef __cplusplus
}
#endif
