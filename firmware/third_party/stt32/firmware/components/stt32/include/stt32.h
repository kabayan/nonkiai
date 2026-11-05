#pragma once

/*
 * stt32 recognition service (DLS-179 contract, DLS-239 implementation).
 *
 * The host application owns the audio hardware (I2S / codec / amp) and hands
 * 16 kHz mono signed 16-bit PCM to stt32 through read_pcm. stt32 keeps a short
 * PCM ring, cuts a fixed 3 s window (after the 300 ms start guard for the
 * one-shot modes, 0.5 s before an RMS onset for CONTINUOUS), runs the backend
 * (log-mel, INT8 inference) on it, applies the score/margin decision, and
 * reports every window through on_result. There is no AFE and no VAD. It owns
 * no FSM and no playback. The wake word (STT32_LISTEN_WAKE) is judged by the same model:
 * the probability of the 18th class (のんきちゃん, DLS-251 / DLS-1082); stt32 only notifies it.
 *
 * Contract: docs/stt32_notif_integration_interface.md
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STT32_STATE_UNINITIALIZED = 0,
    STT32_STATE_READY,
    STT32_STATE_LISTENING,
    STT32_STATE_STOPPING,
    STT32_STATE_ERROR,
} stt32_state_t;

typedef enum {
    STT32_LISTEN_CONTINUOUS = 0,
    STT32_LISTEN_ONE_COMMAND,
    STT32_LISTEN_ONE_UTTERANCE,
    /* Wait for the wake word only (DLS-251 / DLS-1082). Windows are cut like CONTINUOUS (0.5 s before an
     * RMS onset, 3 s). A window whose wake-word probability (class STT32_INTENT_NONKICHAN) is >=
     * stt32_config_t.wake_thresholds.min_score (and, when stt32_config_t.wake_max_rank > 0, whose rank among
     * the classes is <= wake_max_rank) is accepted and reported once with event = STT32_EVENT_WAKE;
     * every other window is not reported. stt32_listen_start returns ESP_ERR_NOT_SUPPORTED when the model has
     * no such class (17 classes). */
    STT32_LISTEN_WAKE,
} stt32_listen_mode_t;

/* What happens after the wake word was accepted (mode == STT32_LISTEN_WAKE only). */
typedef enum {
    STT32_AFTER_WAKE_STOP = 0,   /* stop listening, then report WAKE with listening_stopped = true (default) */
    STT32_AFTER_WAKE_UTTERANCE,  /* report WAKE with listening_stopped = false and judge the next utterance of
                                  * the same session as a command (stops after that one result, like
                                  * ONE_UTTERANCE) with the intent_mask / accept_rule of the options */
} stt32_after_wake_t;

/* What a result is about. */
typedef enum {
    STT32_EVENT_UTTERANCE = 0, /* the result of a command utterance (every result before WAKE existed) */
    STT32_EVENT_WAKE,          /* the wake word was accepted */
} stt32_event_t;

typedef enum {
    STT32_REJECTED = 0,
    STT32_SEMANTIC_ONLY,
    STT32_EXPRESSION_CONFIRMED,
} stt32_decision_t;

typedef enum {
    STT32_REJECT_NONE = 0,
    STT32_REJECT_LOW_SCORE,
    STT32_REJECT_LOW_MARGIN,
    STT32_REJECT_TOO_SHORT,
    STT32_REJECT_INFERENCE_ERROR,
    STT32_REJECT_OUT_OF_SET, /* accept_rule TOP1_IN_SET: the top-1 class of the whole model is not in intent_mask */
    STT32_REJECT_LOW_RANK,   /* WAKE: the wake word's probability is enough but its rank is worse than wake_max_rank */
} stt32_reject_reason_t;

/* Stable ids: semantic_id is a stt32_intent_t (0..17, stt32_intents.h, DLS-240 / DLS-1082),
 * never the model's softmax index. */
typedef uint16_t stt32_semantic_id_t;
typedef uint16_t stt32_expression_id_t;

#define STT32_SEMANTIC_ID_INVALID UINT16_MAX
#define STT32_EXPRESSION_ID_INVALID UINT16_MAX

/* How a window is accepted against the candidate set (stt32_listen_options_t.intent_mask). The probabilities
 * are never renormalised over the set (DLS-003): a score is always the model's own softmax output. */
typedef enum {
    /* 0: as before. semantic_thresholds (min_score, then min_margin) on the top-1 class of the whole model;
     * intent_mask is NOT used. */
    STT32_ACCEPT_THRESHOLDS = 0,
    /* Accept when the top-1 class of the whole model is in intent_mask. Otherwise REJECTED /
     * STT32_REJECT_OUT_OF_SET. semantic_thresholds are not used. */
    STT32_ACCEPT_TOP1_IN_SET,
    /* Accept when the largest probability inside intent_mask is >= set_min_score. Otherwise REJECTED /
     * STT32_REJECT_LOW_SCORE. semantic_thresholds are not used. */
    STT32_ACCEPT_SET_MAX_SCORE,
} stt32_accept_rule_t;

/* A zeroed struct (or options == NULL) behaves exactly as before the fields below existed. */
typedef struct {
    stt32_listen_mode_t mode;
    /* mode == STT32_LISTEN_WAKE only. */
    stt32_after_wake_t after_wake;
    /* Candidate set: bit i = stt32_intent_t i. 0 = all classes. Used by accept_rule TOP1_IN_SET and
     * SET_MAX_SCORE (and by the command of AFTER_WAKE_UTTERANCE); ignored by STT32_ACCEPT_THRESHOLDS. */
    uint32_t intent_mask;
    stt32_accept_rule_t accept_rule;
    /* STT32_ACCEPT_SET_MAX_SCORE: the threshold on the largest in-set probability. */
    float set_min_score;
    /* End detection (DLS-1091). 0 (default) = off: every window is the fixed 3 s. > 0: once the window has an onset,
     * a 20 ms chunk whose mean square is at or below (background * 10^(end_margin_db / 10)) for end_hold_ms
     * (rounded down to 20 ms, at least 20) in a row closes the window there, and it is inferred with its shorter
     * length; the longest stays 3 s. The end counts only after 200 ms (10 chunks) in total of non-quiet chunks since
     * the onset (a click is not a word); until then the window runs on, to 3 s at most. The background is the one
     * the onset was judged against. WAKE and CONTINUOUS have the onset of their window; ONE_UTTERANCE / ONE_COMMAND
     * judge the end only after a chunk of the window exceeded background + 12 dB; their background comes from the
     * start guard (first window) or the quiet end of the previous window, then the window's own chunks (a window with
     * no voice runs the full 3 s). A window that ends while the previous one is still in inference is not dropped: it
     * keeps recording and is handed over when the model is free, with the length found (still busy at 3 s: dropped with a warning).
     * Valid: end_hold_ms 0..2000; end_margin_db 0 <= x < 12 (checked only when end_hold_ms > 0; the onset is
     * background + 12 dB). Reference values from the PC evaluation: 500 ms / 6 dB. */
    uint16_t end_hold_ms;
    float end_margin_db;
} stt32_listen_options_t;

typedef struct {
    stt32_event_t event; /* first field; 0 = a command result */
    uint32_t session_id;
    uint32_t utterance_id;
    stt32_decision_t decision;
    stt32_reject_reason_t reject_reason;

    stt32_semantic_id_t semantic_id;
    stt32_expression_id_t expression_id;
    /* Top-1 softmax score and top-1 minus top-2, filled for rejections too
     * (0 when the utterance never reached the model). With intent_mask and accept_rule TOP1_IN_SET /
     * SET_MAX_SCORE: semantic_id is the largest in-set intent, semantic_score its raw probability and
     * semantic_margin the in-set top-1 minus the in-set top-2 (a one-member set: minus 0). For a
     * TOP1_IN_SET rejection they are the model's overall top-1 and margin. event == WAKE: semantic_id is
     * INVALID, semantic_score the wake-word probability, semantic_margin that minus the largest other class. */
    float semantic_score;
    float semantic_margin;
    float expression_score;
    float expression_margin;

    bool listening_stopped;
    int64_t speech_end_us;
    int64_t result_us;
} stt32_result_t;

typedef void (*stt32_result_callback_t)(void *context, const stt32_result_t *result);

/* Brackets every backend inference: busy = true just before it starts, false
 * right after it returns (before on_result for that window). Always called as
 * a true/false pair; not called when no window is inferred. Called on the
 * infer task: keep it short and do not block. */
typedef void (*stt32_busy_callback_t)(void *context, bool busy);

/* Fill exactly sample_count samples (blocking). Return ESP_OK on success. */
typedef esp_err_t (*stt32_pcm_read_t)(void *context, int16_t *samples, size_t sample_count);

/* Drop PCM buffered on the host side (called while stopping). */
typedef esp_err_t (*stt32_pcm_flush_t)(void *context);

/* Semantic acceptance: top-1 score >= min_score, then top-1 minus top-2 >=
 * min_margin (same order as common/src/recognition_filter.cpp). The values are
 * model-specific and are not calibrated for an FSM-free whole-vocabulary use;
 * the host chooses them. */
typedef struct {
    float min_score;
    float min_margin;
} stt32_thresholds_t;

/* DEFAULT (0, a zeroed config) is treated as GRU. */
typedef enum {
    STT32_WEIGHTS_COPY_DEFAULT = 0,
    STT32_WEIGHTS_COPY_NONE, /* read flash directly */
    STT32_WEIGHTS_COPY_GRU,  /* the 2-layer bidirectional GRU weights only (614,400 B) */
    STT32_WEIGHTS_COPY_ALL,  /* the whole weight blob */
} stt32_weights_copy_t;

typedef struct {
    stt32_pcm_read_t read_pcm;
    stt32_pcm_flush_t flush_pcm;
    void *pcm_context;
    stt32_result_callback_t on_result;
    void *callback_context;
    stt32_thresholds_t semantic_thresholds;

    /* Everything below: 0 / NULL selects the default. */
    /* heap_caps flags for the PCM ring, the window buffer and the backend's
     * work buffers. Default MALLOC_CAP_SPIRAM; ignored on the linux target. */
    uint32_t work_caps;
    /* Inference task (runs on_result). Default core 0, priority 5. */
    int infer_core;
    int infer_priority;
    /* Second compute task of the backend; stt32 only passes it on. Default
     * core 1, priority 6 (so core 0 cannot be requested explicitly). */
    int worker_core;
    int worker_priority;
    /* Partition label holding the model weights; NULL: linked into .rodata. */
    const char *weights_partition;
    /* Which model weights init() copies from flash into work_caps memory (the GRU re-reads
     * its weights every step and the flash cache thrashes). 0 = STT32_WEIGHTS_COPY_GRU.
     * init fails with ESP_ERR_NO_MEM if the copy does not fit. */
    stt32_weights_copy_t weights_copy;
    /* Inference start / end notification; NULL (default) = none. Receives
     * callback_context, runs on the infer task: short, non-blocking. */
    stt32_busy_callback_t on_busy;
    /* Wake-word acceptance (STT32_LISTEN_WAKE): min_score only (min_margin is not used). Compared with the
     * probability of STT32_INTENT_NONKICHAN with >=, so a zeroed value accepts every window that has speech:
     * the host must set it. */
    stt32_thresholds_t wake_thresholds;
    /* Wake-word rank limit (STT32_LISTEN_WAKE): 0 = the rank is not looked at (default). n > 0: the window is
     * accepted only if the rank of STT32_INTENT_NONKICHAN is <= n as well as its probability >= min_score.
     * rank = 1 + the number of classes whose probability is strictly greater than the wake word's (a tie
     * favours the wake word). Valid 0..STT32_INTENT_COUNT; more: stt32_init returns ESP_ERR_INVALID_ARG. A
     * window rejected for its rank is not reported (like any non-accepted WAKE window). */
    uint32_t wake_max_rank;
} stt32_config_t;

esp_err_t stt32_init(const stt32_config_t *config);

esp_err_t stt32_listen_start(const stt32_listen_options_t *options, uint32_t *session_id_out);

esp_err_t stt32_listen_stop(uint32_t timeout_ms);

stt32_state_t stt32_get_state(void);

esp_err_t stt32_deinit(void);

#ifdef __cplusplus
}
#endif
