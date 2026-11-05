/*
 * stt32 service lifecycle (DLS-179 contract, DLS-239, DLS-242/243).
 *
 * pump_task  : host read_pcm (20 ms chunks) -> PCM ring + 20 ms mean-square ->
 *              3 s window (drops the 300 ms start guard) -> window buffer
 * infer_task : window -> backend inference -> score/margin decision -> on_result
 *
 * Windows (STT32_WINDOW_SAMPLES = 48000):
 *   ONE_UTTERANCE / ONE_COMMAND : the first starts where the start guard ends;
 *       the next starts right after the previous one (ONE_COMMAND continues
 *       after a REJECTED result, ONE_UTTERANCE stops after the first result).
 *   WAKE : windows are cut exactly like CONTINUOUS (below). Only the wake-word probability (class
 *       STT32_INTENT_NONKICHAN) is judged against wake_thresholds.min_score; a window that is not accepted
 *       produces no result at all. With after_wake = STOP the accepted window stops the session before the
 *       WAKE result; with UTTERANCE the session goes on (WAKE with listening_stopped = false) and the next
 *       window is judged as a command, then the session stops, like ONE_UTTERANCE.
 *   CONTINUOUS : a window opens STT32_LEAD_SAMPLES (0.5 s) before an onset. Onset
 *       = the mean square of one 20 ms chunk exceeds background * 10^(12/10),
 *       where background = the 10th percentile (nearest rank) of the mean
 *       squares of the last 50 chunks (1 s) taken while no window was open,
 *       floored at STT32_BG_FLOOR_MS (rms 2). 12 dB on amplitude equals a factor
 *       10^(12/10) = 15.85 on mean square, so no sqrt or log is needed. At
 *       least STT32_BG_MIN_CHUNKS (100 ms) of history are required, and an
 *       onset earlier than STT32_LEAD_SAMPLES (0.5 s) after the guard ended is
 *       not detected (it only enters the history), so the onset is always the
 *       8000th sample of the window.
 *
 * End detection (stt32_listen_options_t.end_hold_ms > 0, default off = the fixed 3 s above): after the onset
 * of an onset window, once the mean square of a 20 ms chunk is at or below background * 10^(end_margin_db/10)
 * for end_hold_ms in a row, the window closes at the end of that run and is inferred with its shorter length
 * (the longest stays 3 s). The background is the one of the onset, frozen. The end counts only after
 * STT32_END_MIN_VOICED_MS of non-quiet chunks since the onset (a click is not a word). ONE_UTTERANCE / ONE_COMMAND
 * have no onset: the end is judged only after a chunk of the window exceeded the onset condition (background +
 * 12 dB), against the 10th percentile of the history, at least 100 ms of it: the start guard (first window), the
 * quiet run that closed the previous window (a window closed by the 3 s timer leaves nothing), then the window's
 * own chunks seen before the onset. A window with no voice runs the full 3 s.
 * If the end is found while the previous window is still in inference, the window is not dropped: it keeps
 * recording and is handed over with the remembered length when the model is free (if the model is still busy at 3 s, it is dropped with a warning as before). With
 * end detection an onset that arrives during inference is therefore kept too.
 *
 * Without end detection a window or onset that appears while the window buffer is still in inference
 * is dropped with a warning. Public calls are serialized by s_api_lock. The
 * pump only captures while `run` is set for the current session; stopping
 * clears it, waits until the pump is parked, drops queued and in-flight work and
 * only then flushes host PCM. A callback is delivered under s_deliver_lock
 * after a session check, so once stop returns no callback of that session can
 * start.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "stt32.h"
#include "stt32_backend.h"
#include "stt32_policy.h"
#include "stt32_intents.h"
#if CONFIG_STT32_DIAG_WINDOW
#include "stt32_diag.h"
#endif

#if CONFIG_IDF_TARGET_LINUX
#include <time.h>
#else
#include "esp_heap_caps.h"
#include "esp_timer.h"
#endif

static const char *TAG = "stt32";

#define STT32_SAMPLE_RATE 16000
#define STT32_CHUNK_SAMPLES 320 /* 20 ms, the unit of read_pcm and of the RMS */
#define STT32_RING_SAMPLES 56000 /* 3.5 s */
#define STT32_LEAD_SAMPLES 8000  /* CONTINUOUS: window starts 0.5 s before the onset */
/* Contract §6.2: discard input for 300 ms after each start (half-duplex guard). */
#define STT32_START_GUARD_SAMPLES (STT32_SAMPLE_RATE * 300 / 1000)
#define STT32_BG_HISTORY 50         /* chunks of background history (1 s) */
#define STT32_BG_MIN_CHUNKS 5
#define STT32_BG_FLOOR_MS 4u        /* mean square floor (rms 2) */
#define STT32_ONSET_POWER_X100 1585 /* 10^(12/10) * 100 */
#define STT32_END_HOLD_MAX_MS 2000
#define STT32_END_MARGIN_LIMIT_DB 12.0f /* exclusive: the onset is background + 12 dB */
#define STT32_END_MIN_VOICED_MS 200
#define STT32_END_MIN_VOICED_CHUNKS (STT32_END_MIN_VOICED_MS / 20)
#define STT32_CLOSE_TIMER 0         /* same values as stt32_diag_close_t */
#define STT32_CLOSE_END 1
#define STT32_INFER_POLL_MS 100
#define STT32_DEINIT_TIMEOUT_MS 2000
#define STT32_DEFAULT_INFER_PRIORITY 5
#define STT32_DEFAULT_WORKER_CORE 1
#define STT32_DEFAULT_WORKER_PRIORITY 6

#define BIT_PUMP_PARKED (1u << 0)
#define BIT_PUMP_EXITED (1u << 1)
#define BIT_INFER_EXITED (1u << 2)
#define BITS_PARKED BIT_PUMP_PARKED
#define BITS_EXITED (BIT_PUMP_EXITED | BIT_INFER_EXITED)

typedef struct {
    uint32_t session_id;
    int len;
    int64_t speech_end_us;
    int onset_at;      /* sample of the onset inside the window; -1: none seen */
    uint8_t close_by;  /* STT32_CLOSE_* */
} utt_msg_t;

static SemaphoreHandle_t s_api_lock;

static struct {
    stt32_config_t cfg;
    const stt32_backend_t *be;
    SemaphoreHandle_t lock;         /* fields below shared with the workers */
    SemaphoreHandle_t deliver_lock; /* held for the session check + callback */
    EventGroupHandle_t ev;
    QueueHandle_t utt_q;
    TaskHandle_t pump;
    TaskHandle_t infer;
    int16_t *chunk;  /* internal DRAM: one read_pcm chunk */
    int16_t *ring;   /* work_caps: STT32_RING_SAMPLES */
    int16_t *window; /* work_caps: STT32_WINDOW_SAMPLES, handed to infer_task */
    float *scores;
    size_t class_count;
    uint32_t work_caps;

    volatile stt32_state_t state;
    volatile bool run;          /* the pump may capture for run_session */
    volatile bool exiting;
    volatile bool utt_busy;     /* window buffer handed to infer_task */
    volatile int guard_samples;
    uint32_t run_session;
    uint32_t auto_session;      /* auto-stopped session whose result is pending */
    stt32_listen_mode_t mode;
    stt32_listen_options_t opt; /* options of the running session (mode included) */
    bool wake_command;          /* WAKE + UTTERANCE: the wake word was accepted, the next window is a command */
    uint32_t last_session_id;
    uint32_t utterance_seq;
#if CONFIG_STT32_DIAG_WINDOW
    uint32_t window_seq; /* windows handed to the model in the running session */
#endif
} s;

#if CONFIG_STT32_DIAG_WINDOW
/* fn and ctx are published as one immutable pair: the setter allocates a new pair and swaps the pointer, the infer
 * task loads the pointer once per window. A reader therefore never sees the fn of one call with the ctx of another.
 * The replaced pair is deliberately not freed (a window in flight may still be using it): 8 bytes per set call,
 * and the set calls are rare. Set survives deinit: it belongs to the host, not to the service state. */
typedef struct {
    stt32_diag_window_hook_t fn;
    void *ctx;
} diag_hook_t;
static diag_hook_t *s_diag;

void stt32_diag_set_window_hook(stt32_diag_window_hook_t hook, void *context) {
    diag_hook_t *h = NULL;
    if (hook != NULL) {
        h = (diag_hook_t *)malloc(sizeof(*h));
        if (h == NULL) {
            return; /* keep the previous hook */
        }
        h->fn = hook;
        h->ctx = context;
    }
    __atomic_store_n(&s_diag, h, __ATOMIC_RELEASE);
}
#endif

static int64_t now_us(void) {
#if CONFIG_IDF_TARGET_LINUX
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#else
    return esp_timer_get_time();
#endif
}

#if CONFIG_IDF_TARGET_LINUX
/* Test hook: largest request that would be internal DRAM on the device. */
static size_t s_max_internal_alloc;
size_t stt32_service_test_max_internal_alloc(void) {
    return s_max_internal_alloc;
}
#endif

/* 16-byte aligned. `internal` selects internal DRAM; otherwise caps (work_caps). */
static void *alloc_caps(size_t bytes, bool internal, uint32_t caps) {
#if CONFIG_IDF_TARGET_LINUX
    (void)caps;
    if (internal && bytes > s_max_internal_alloc) {
        s_max_internal_alloc = bytes;
    }
    void *p = NULL;
    return posix_memalign(&p, 16, bytes) == 0 ? p : NULL;
#else
    return heap_caps_aligned_alloc(16, bytes, internal ? MALLOC_CAP_INTERNAL : caps);
#endif
}

static void free_caps(void *p) {
#if CONFIG_IDF_TARGET_LINUX
    free(p);
#else
    heap_caps_free(p);
#endif
}

static void lock(void) {
    xSemaphoreTake(s.lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGive(s.lock);
}

/* Single-core targets (and the linux test port) have only core 0. */
static BaseType_t core_or_any(BaseType_t core) {
    return (core < configNUMBER_OF_CORES) ? core : tskNO_AFFINITY;
}

static bool called_from_worker(void) {
    const TaskHandle_t self = xTaskGetCurrentTaskHandle();
    return self == s.infer || self == s.pump;
}

/* ---- workers ---------------------------------------------------------- */

typedef struct {
    int64_t total;      /* samples written to the ring since the guard ended */
    bool started;       /* mode and session are latched at the first chunk */
    stt32_listen_mode_t mode;
    uint32_t session;
    bool win_open;
    int64_t win_start;  /* window = [win_start, win_start + WINDOW) */
    int64_t skip_until; /* CONTINUOUS: onsets before this sample are ignored */
    uint32_t hist[STT32_BG_HISTORY]; /* mean squares of chunks seen with no window open */
    int hist_n;
    int hist_pos;
    /* End detection (opt.end_hold_ms > 0) */
    int end_hold;       /* chunks, 0 = off */
    double end_mul;     /* 10^(end_margin_db/10) */
    bool end_armed;     /* an onset exists in the open window */
    uint64_t end_thr100; /* background * end_mul * 100, frozen at the onset */
    int end_run;        /* consecutive chunks at or below the threshold since the onset */
    int onset_at;       /* sample of the onset inside the window, -1: none */
    int voiced;         /* chunks above the end threshold since the onset (the onset chunk included) */
    int64_t fed;        /* one-shot: the window has seen the samples up to here */
    int64_t pend_len;   /* > 0: the end is found, the window waits for the model to be free */
    uint8_t pend_by;
    int64_t pend_us;
    uint32_t tail[STT32_BG_HISTORY]; /* mean squares of the last chunks seen after the onset */
    int tail_n;
    int tail_pos;
} capture_t;

/* CONTINUOUS and WAKE cut windows at an RMS onset; the one-shot modes take them back to back. */
static bool onset_mode(stt32_listen_mode_t mode) {
    return mode == STT32_LISTEN_CONTINUOUS || mode == STT32_LISTEN_WAKE;
}

static void ring_write(const int16_t *pcm, int count, int64_t at) {
    size_t pos = (size_t)(at % STT32_RING_SAMPLES);
    for (int i = 0; i < count; ++i) {
        s.ring[pos] = pcm[i];
        if (++pos == STT32_RING_SAMPLES) {
            pos = 0;
        }
    }
}

static uint32_t mean_square(const int16_t *pcm, int count) {
    uint64_t sum = 0;
    for (int i = 0; i < count; ++i) {
        sum += (uint64_t)((int32_t)pcm[i] * (int32_t)pcm[i]);
    }
    return (uint32_t)(sum / (uint64_t)count);
}

/* 10th percentile (nearest rank) of the history, floored. */
static uint32_t background_ms(const capture_t *c) {
    uint32_t v[STT32_BG_HISTORY];
    const int n = c->hist_n;
    for (int i = 0; i < n; ++i) {
        uint32_t x = c->hist[i];
        int j = i;
        while (j > 0 && v[j - 1] > x) {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = x;
    }
    uint32_t bg = v[(n * 10 + 99) / 100 - 1];
    return bg > STT32_BG_FLOOR_MS ? bg : STT32_BG_FLOOR_MS;
}

/* background * 10^(margin/10) * 100, the threshold of "quiet" in units of mean_square * 100. */
static uint64_t end_threshold100(const capture_t *c, uint32_t bg) {
    return (uint64_t)((double)bg * c->end_mul * 100.0 + 0.5);
}

static void history_push(capture_t *c, uint32_t ms) {
    c->hist[c->hist_pos] = ms;
    c->hist_pos = (c->hist_pos + 1) % STT32_BG_HISTORY;
    if (c->hist_n < STT32_BG_HISTORY) {
        c->hist_n++;
    }
}

/* The window is complete: copy it out of the ring and hand it to infer_task. */
static void close_window(capture_t *c, int len, uint8_t close_by, int64_t end_us) {
    c->win_open = false;
    if (s.utt_busy) {
        ESP_LOGW(TAG, "window dropped: previous one still in inference");
        return;
    }
    size_t pos = (size_t)(c->win_start % STT32_RING_SAMPLES);
    for (int i = 0; i < len; ++i) {
        s.window[i] = s.ring[pos];
        if (++pos == STT32_RING_SAMPLES) {
            pos = 0;
        }
    }
    const utt_msg_t m = {c->session, len, end_us, c->onset_at, close_by};
    lock();
    if (s.run && c->session == s.run_session) {
        s.utt_busy = true;
        xQueueSend(s.utt_q, &m, 0);
    }
    unlock();
}

/* Mean square of the chunk that starts at `at`, read back from the ring. */
static uint32_t ring_mean_square(int64_t at) {
    uint64_t sum = 0;
    size_t pos = (size_t)(at % STT32_RING_SAMPLES);
    for (int i = 0; i < STT32_CHUNK_SAMPLES; ++i) {
        sum += (uint64_t)((int32_t)s.ring[pos] * (int32_t)s.ring[pos]);
        if (++pos == STT32_RING_SAMPLES) {
            pos = 0;
        }
    }
    return (uint32_t)(sum / STT32_CHUNK_SAMPLES);
}

static void tail_push(capture_t *c, uint32_t ms) {
    c->tail[c->tail_pos] = ms;
    c->tail_pos = (c->tail_pos + 1) % STT32_BG_HISTORY;
    if (c->tail_n < STT32_BG_HISTORY) {
        c->tail_n++;
    }
}

/* The next one-shot window judges its background from the quiet chunks that ended the previous window (the run
 * that closed it, at most 1 s), so a voice at its very start is not taken for the background. A window closed by
 * the 3 s timer ends in an unknown state: the next one starts with an empty history and uses its own first chunks. */
static void next_window_history(capture_t *c, uint8_t close_by) {
    c->hist_n = 0;
    c->hist_pos = 0;
    if (close_by == STT32_CLOSE_END) {
        const int n = c->end_hold < c->tail_n ? c->end_hold : c->tail_n;
        for (int i = 0; i < n; ++i) {
            history_push(c, c->tail[(c->tail_pos - n + i + STT32_BG_HISTORY) % STT32_BG_HISTORY]);
        }
    }
}

static void finish_close(capture_t *c, int64_t len, uint8_t close_by, int64_t end_us) {
    const int64_t end = c->win_start + len;
    close_window(c, (int)len, close_by, end_us);
    c->pend_len = 0;
    if (!onset_mode(c->mode)) {
        c->win_open = true; /* the next window follows directly; its history was prepared when the close was decided */
        c->win_start = end;
        c->fed = end;
        c->end_armed = false;
        c->end_run = 0;
        c->voiced = 0;
        c->onset_at = -1;
    }
}

/* One chunk (ending at `tot`) of the open window: end detection and closing. */
static void window_step(capture_t *c, uint32_t ms, int64_t tot, bool opened_now) {
    c->fed = tot;
    if (c->pend_len > 0) {
        /* The end was found while the previous window was still in inference: keep recording, nothing is dropped.
         * Hand over the remembered length as soon as the model is free, at the latest at the 3 s mark. */
        if (!s.utt_busy || tot >= c->win_start + STT32_WINDOW_SAMPLES) {
            finish_close(c, c->pend_len, c->pend_by, c->pend_us);
        }
        return;
    }
    int64_t close_len = 0; /* > 0: close now with this many samples */
    uint8_t close_by = STT32_CLOSE_TIMER;
    if (c->end_hold > 0 && !opened_now) {
        if (!c->end_armed) {
            /* One-shot window: wait for the first chunk above background + 12 dB (judged against the chunks seen
             * before it: the start guard, the end of the previous window and this window's own chunks). */
            if (c->hist_n >= STT32_BG_MIN_CHUNKS &&
                (uint64_t)ms * 100 > (uint64_t)background_ms(c) * STT32_ONSET_POWER_X100) {
                c->end_armed = true;
                c->end_run = 0;
                c->voiced = 1;
                c->tail_n = 0;
                c->end_thr100 = end_threshold100(c, background_ms(c));
                c->onset_at = (int)(tot - STT32_CHUNK_SAMPLES - c->win_start);
            } else {
                history_push(c, ms);
            }
        } else {
            tail_push(c, ms);
            if ((uint64_t)ms * 100 <= c->end_thr100) {
                /* A click is not a word: the end counts only after STT32_END_MIN_VOICED_MS of non-quiet chunks. */
                if (c->voiced >= STT32_END_MIN_VOICED_CHUNKS && ++c->end_run >= c->end_hold) {
                    close_len = tot - c->win_start;
                    close_by = STT32_CLOSE_END;
                }
            } else {
                c->end_run = 0;
                c->voiced++;
            }
        }
    }
    if (close_len == 0 && tot >= c->win_start + STT32_WINDOW_SAMPLES) {
        close_len = STT32_WINDOW_SAMPLES;
    }
    if (close_len > 0) {
        if (!onset_mode(c->mode)) {
            next_window_history(c, close_by);
        }
        if (close_by == STT32_CLOSE_END && s.utt_busy) {
            c->pend_len = close_len;
            c->pend_by = close_by;
            c->pend_us = now_us(); /* the speech ended now, whenever the window can be handed over */
            return;
        }
        finish_close(c, close_len, close_by, now_us());
    }
}

static void capture_init(capture_t *c) {
    c->started = true;
    lock();
    c->mode = s.mode;
    c->session = s.run_session;
    const stt32_listen_options_t opt = s.opt;
    unlock();
    c->end_hold = 0;
    c->end_mul = 1.0;
    if (opt.end_hold_ms > 0) {
        c->end_hold = opt.end_hold_ms / 20 > 0 ? opt.end_hold_ms / 20 : 1;
        c->end_mul = pow(10.0, (double)opt.end_margin_db / 10.0);
    }
    c->onset_at = -1;
    if (!onset_mode(c->mode)) {
        c->win_open = true;
        c->win_start = 0;
    }
}

/* A chunk of the start guard: dropped, except that an end-detecting one-shot window takes it as the first
 * background of its first window (the guard is the quiet before the speaker is expected to speak). */
static void capture_guard(capture_t *c, const int16_t *pcm) {
    if (!c->started) {
        capture_init(c);
    }
    if (c->end_hold > 0 && !onset_mode(c->mode)) {
        history_push(c, mean_square(pcm, STT32_CHUNK_SAMPLES));
    }
}

static void capture_chunk(capture_t *c, const int16_t *pcm) {
    if (!c->started) {
        capture_init(c);
    }
    const int64_t at = c->total;
    ring_write(pcm, STT32_CHUNK_SAMPLES, at);
    c->total += STT32_CHUNK_SAMPLES;
    const uint32_t ms = mean_square(pcm, STT32_CHUNK_SAMPLES);
    bool opened_now = false;
    if (!c->win_open) { /* CONTINUOUS and WAKE only */
        bool onset = false;
        /* An onset needs a full lead (0.5 s) since the guard ended: the window then
         * always starts at at - STT32_LEAD_SAMPLES >= 0 and the onset sits at its
         * 8000th sample. Earlier sound only feeds the history. */
        if (at >= c->skip_until && at >= STT32_LEAD_SAMPLES && c->hist_n >= STT32_BG_MIN_CHUNKS) {
            onset = (uint64_t)ms * 100 > (uint64_t)background_ms(c) * STT32_ONSET_POWER_X100;
        }
        if (!onset) {
            history_push(c, ms);
            return;
        }
        const int64_t start = at - STT32_LEAD_SAMPLES;
        if (s.utt_busy && c->end_hold == 0) {
            /* Fixed 3 s windows: the window this onset would open runs to start + 3 s, and is dropped. With end
             * detection the onset is kept: the window may close early, and if the model is still busy then the close
             * is held back (window_step), so nothing is dropped here. */
            ESP_LOGW(TAG, "onset dropped: previous window still in inference");
            c->skip_until = start + STT32_WINDOW_SAMPLES;
            return;
        }
        c->win_open = true;
        c->win_start = start;
        opened_now = true;
        c->end_run = 0;
        c->voiced = 1;
        c->tail_n = 0;
        c->onset_at = STT32_LEAD_SAMPLES;
        c->end_armed = c->end_hold > 0;
        if (c->end_armed) {
            c->end_thr100 = end_threshold100(c, background_ms(c));
        }
    }
    window_step(c, ms, c->total, opened_now);
    /* A held-back window was handed over late: the next one-shot window began at its end, before this chunk. Feed it
     * the chunks it has already seen (all in the ring, a window is at most 3 s). */
    while (c->win_open && !onset_mode(c->mode) && c->fed + STT32_CHUNK_SAMPLES <= c->total) {
        window_step(c, ring_mean_square(c->fed), c->fed + STT32_CHUNK_SAMPLES, false);
    }
}

static void pump_task(void *arg) {
    (void)arg;
    capture_t cap;
    memset(&cap, 0, sizeof(cap));
    for (;;) {
        if (s.exiting) {
            break;
        }
        if (!s.run) {
            memset(&cap, 0, sizeof(cap));
            xEventGroupSetBits(s.ev, BIT_PUMP_PARKED);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (s.cfg.read_pcm(s.cfg.pcm_context, s.chunk, STT32_CHUNK_SAMPLES) != ESP_OK) {
            vTaskDelay(1);
            continue;
        }
        if (!s.run) {
            continue;
        }
        if (s.guard_samples > 0) {
            s.guard_samples -= STT32_CHUNK_SAMPLES;
            capture_guard(&cap, s.chunk);
            continue;
        }
        capture_chunk(&cap, s.chunk);
    }
    xEventGroupSetBits(s.ev, BIT_PUMP_EXITED);
    vTaskDelete(NULL);
}

/*
 * Park the pump, then drop host PCM. The caller has set state = STOPPING and
 * run = false under the lock.
 */
static esp_err_t park_and_reset(TickType_t timeout) {
    xTaskNotifyGive(s.pump);
    const EventBits_t bits = xEventGroupWaitBits(s.ev, BITS_PARKED, pdFALSE, pdTRUE, timeout);
    if ((bits & BITS_PARKED) != BITS_PARKED) {
        return ESP_ERR_TIMEOUT;
    }
    s.cfg.flush_pcm(s.cfg.pcm_context);
    return ESP_OK;
}

static void begin_stop_locked(void) {
    s.state = STT32_STATE_STOPPING;
    s.run = false;
    xEventGroupClearBits(s.ev, BITS_PARKED);
}

/* Auto-stop from infer_task (contract §5): stop before the callback. */
static bool auto_stop(uint32_t session_id) {
    /* Poll so a manual stop or deinit holding the API lock is never waited on. */
    while (xSemaphoreTake(s_api_lock, pdMS_TO_TICKS(10)) != pdTRUE) {
        if (s.exiting || !s.run) {
            return false;
        }
    }
    lock();
    const bool current = s.state == STT32_STATE_LISTENING && s.run_session == session_id;
    if (current) {
        begin_stop_locked();
    }
    unlock();
    bool stopped = false;
    if (current) {
        const esp_err_t err = park_and_reset(portMAX_DELAY);
        lock();
        s.state = (err == ESP_OK) ? STT32_STATE_READY : STT32_STATE_ERROR;
        s.run_session = 0;
        s.auto_session = session_id;
        unlock();
        stopped = err == ESP_OK;
    }
    xSemaphoreGive(s_api_lock);
    return stopped;
}

static void deliver(const stt32_result_t *r) {
    xSemaphoreTake(s.deliver_lock, portMAX_DELAY);
    lock();
    bool ok;
    if (r->listening_stopped) {
        ok = s.auto_session == r->session_id;
        s.auto_session = 0;
    } else {
        ok = s.run && s.state == STT32_STATE_LISTENING && s.run_session == r->session_id;
    }
    unlock();
    if (ok) {
        s.cfg.on_result(s.cfg.callback_context, r);
    }
    xSemaphoreGive(s.deliver_lock);
}

static void infer_task(void *arg) {
    (void)arg;
    for (;;) {
        if (s.exiting) {
            break;
        }
        utt_msg_t m;
        if (xQueueReceive(s.utt_q, &m, pdMS_TO_TICKS(STT32_INFER_POLL_MS)) != pdTRUE) {
            continue;
        }
        lock();
        const bool current = s.run && s.run_session == m.session_id;
        const stt32_listen_mode_t mode = s.mode;
        const stt32_listen_options_t opt = s.opt;
        const bool wake_command = s.wake_command;
        unlock();
        /* WAKE judges the wake word until it is accepted; a window that is not accepted is not reported. */
        const bool wake_judging = mode == STT32_LISTEN_WAKE && !wake_command;
        stt32_result_t r;
        memset(&r, 0, sizeof(r));
#if CONFIG_STT32_DIAG_WINDOW
        int diag_rc = -1;
        int64_t diag_t0 = 0;
        uint32_t diag_us = 0;
#endif
        if (current) {
            if (s.cfg.on_busy != NULL) {
                s.cfg.on_busy(s.cfg.callback_context, true);
            }
#if CONFIG_STT32_DIAG_WINDOW
            diag_t0 = now_us();
#endif
            const int rc = s.be->infer(s.window, (size_t)m.len, s.scores, s.class_count);
#if CONFIG_STT32_DIAG_WINDOW
            diag_us = (uint32_t)(now_us() - diag_t0);
            diag_rc = rc;
#endif
            if (s.cfg.on_busy != NULL) {
                s.cfg.on_busy(s.cfg.callback_context, false);
            }
            if (rc == 0 && wake_judging) {
                stt32_policy_decide_wake(s.scores, s.class_count, s.be->semantic_ids(), s.cfg.wake_thresholds, s.cfg.wake_max_rank, &r);
            } else if (rc == 0) {
                stt32_policy_decide_set(s.scores, s.class_count, s.be->semantic_ids(), s.cfg.semantic_thresholds,
                                        &opt, &r);
            } else {
                stt32_policy_reject(rc == 1 ? STT32_REJECT_TOO_SHORT : STT32_REJECT_INFERENCE_ERROR, &r);
            }
        }
        /* WAKE: nothing to report for a window that did not carry the wake word. */
        const bool silent = wake_judging && r.decision == STT32_REJECTED;
        lock();
        s.utt_busy = false;
        const bool still_current = current && s.run && s.run_session == m.session_id;
        if (still_current && !silent) {
            r.utterance_id = ++s.utterance_seq;
            if (wake_judging && opt.after_wake == STT32_AFTER_WAKE_UTTERANCE) {
                s.wake_command = true; /* the next window is the command */
            }
        }
#if CONFIG_STT32_DIAG_WINDOW
        const uint32_t diag_window = current ? ++s.window_seq : 0;
#endif
        unlock();
#if CONFIG_STT32_DIAG_WINDOW
        const diag_hook_t *diag_h = __atomic_load_n(&s_diag, __ATOMIC_ACQUIRE); /* once: fn and ctx stay a pair */
        if (current && diag_h != NULL) {
            stt32_diag_window_t w;
            memset(&w, 0, sizeof(w));
            w.session_id = m.session_id;
            w.window_index = diag_window;
            w.utterance_id = r.utterance_id;
            w.mode = mode;
            w.after_wake = opt.after_wake;
            w.wake_judging = wake_judging;
            w.infer_rc = diag_rc;
            if (diag_rc == 0) {
                w.scores = s.scores;
                w.semantic_ids = s.be->semantic_ids();
            }
            w.class_count = s.class_count;
            w.event = r.event;
            w.decision = r.decision;
            w.reject_reason = r.reject_reason;
            w.semantic_id = r.semantic_id;
            w.semantic_score = r.semantic_score;
            w.semantic_margin = r.semantic_margin;
            w.reported = still_current && !silent;
            w.speech_end_us = m.speech_end_us;
            w.window_samples = (uint32_t)m.len;
            w.close_by = m.close_by == STT32_CLOSE_END ? STT32_DIAG_CLOSE_END : STT32_DIAG_CLOSE_TIMER;
            w.onset_sample = m.onset_at;
            w.infer_start_us = diag_t0;
            w.infer_us = diag_us;
            diag_h->fn(diag_h->ctx, &w);
        }
#endif
        if (!still_current || silent) {
            continue;
        }
        r.session_id = m.session_id;
        r.speech_end_us = m.speech_end_us;
        const bool stops = wake_judging ? opt.after_wake == STT32_AFTER_WAKE_STOP
                                        : (mode == STT32_LISTEN_WAKE || stt32_policy_stops(mode, r.decision));
        if (stops) {
            if (!auto_stop(m.session_id)) {
                continue; /* a manual stop got there first */
            }
            r.listening_stopped = true;
        }
        r.result_us = now_us();
        deliver(&r);
    }
    xEventGroupSetBits(s.ev, BIT_INFER_EXITED);
    vTaskDelete(NULL);
}

/* ---- public API -------------------------------------------------------- */

static void release_resources(void) {
    if (s.utt_q != NULL) vQueueDelete(s.utt_q);
    if (s.ev != NULL) vEventGroupDelete(s.ev);
    if (s.lock != NULL) vSemaphoreDelete(s.lock);
    if (s.deliver_lock != NULL) vSemaphoreDelete(s.deliver_lock);
    free_caps(s.chunk);
    free_caps(s.ring);
    free_caps(s.window);
    free_caps(s.scores);
    const stt32_backend_t *be = s.be;
    const uint32_t last_session_id = s.last_session_id; /* stay monotonic across re-init */
    memset(&s, 0, sizeof(s));
    s.state = STT32_STATE_UNINITIALIZED;
    s.last_session_id = last_session_id;
    if (be != NULL) {
        be->deinit();
    }
}

static bool thresholds_valid(stt32_thresholds_t t) {
    return isfinite(t.min_score) && isfinite(t.min_margin);
}

static bool options_valid(const stt32_listen_options_t *o) {
    return (o->mode == STT32_LISTEN_CONTINUOUS || o->mode == STT32_LISTEN_ONE_COMMAND ||
            o->mode == STT32_LISTEN_ONE_UTTERANCE || o->mode == STT32_LISTEN_WAKE) &&
           (o->after_wake == STT32_AFTER_WAKE_STOP || o->after_wake == STT32_AFTER_WAKE_UTTERANCE) &&
           (o->accept_rule == STT32_ACCEPT_THRESHOLDS || o->accept_rule == STT32_ACCEPT_TOP1_IN_SET ||
            o->accept_rule == STT32_ACCEPT_SET_MAX_SCORE) &&
           isfinite(o->set_min_score) && o->end_hold_ms <= STT32_END_HOLD_MAX_MS &&
           (o->end_hold_ms == 0 || (isfinite(o->end_margin_db) && o->end_margin_db >= 0.0f &&
                                    o->end_margin_db < STT32_END_MARGIN_LIMIT_DB));
}

esp_err_t stt32_init(const stt32_config_t *config) {
    if (config == NULL || config->read_pcm == NULL || config->flush_pcm == NULL || config->on_result == NULL ||
        !thresholds_valid(config->semantic_thresholds) || !thresholds_valid(config->wake_thresholds) ||
        config->wake_max_rank > (uint32_t)STT32_INTENT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_api_lock == NULL) {
        s_api_lock = xSemaphoreCreateMutex();
        if (s_api_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    if (s.state != STT32_STATE_UNINITIALIZED) {
        xSemaphoreGive(s_api_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s.cfg = *config;
#if CONFIG_IDF_TARGET_LINUX
    s.work_caps = 0;
#else
    s.work_caps = config->work_caps != 0 ? config->work_caps : MALLOC_CAP_SPIRAM;
#endif
    s.be = stt32_backend_default();
    const stt32_backend_config_t bcfg = {
        s.work_caps,
        config->worker_core != 0 ? config->worker_core : STT32_DEFAULT_WORKER_CORE,
        config->worker_priority != 0 ? config->worker_priority : STT32_DEFAULT_WORKER_PRIORITY,
        (int)core_or_any(config->infer_core),
        config->weights_partition,
        config->weights_copy == STT32_WEIGHTS_COPY_DEFAULT ? STT32_WEIGHTS_COPY_GRU : config->weights_copy,
    };
    esp_err_t err = s.be->init(&bcfg);
    if (err != ESP_OK) {
        s.be = NULL; /* nothing to deinit */
        release_resources();
        xSemaphoreGive(s_api_lock);
        return err;
    }
    s.class_count = s.be->class_count();
    /* Internal DRAM: the chunk and the scores only. The ring and the window
     * live in work_caps. */
    s.scores = (float *)alloc_caps(s.class_count * sizeof(float), true, 0);
    s.chunk = (int16_t *)alloc_caps(STT32_CHUNK_SAMPLES * sizeof(int16_t), true, 0);
    s.ring = (int16_t *)alloc_caps(STT32_RING_SAMPLES * sizeof(int16_t), false, s.work_caps);
    s.window = (int16_t *)alloc_caps(STT32_WINDOW_SAMPLES * sizeof(int16_t), false, s.work_caps);
    s.utt_q = xQueueCreate(1, sizeof(utt_msg_t));
    s.ev = xEventGroupCreate();
    s.lock = xSemaphoreCreateMutex();
    s.deliver_lock = xSemaphoreCreateMutex();
    if (s.scores == NULL || s.chunk == NULL || s.ring == NULL || s.window == NULL || s.utt_q == NULL ||
        s.ev == NULL || s.lock == NULL || s.deliver_lock == NULL) {
        release_resources();
        xSemaphoreGive(s_api_lock);
        return ESP_ERR_NO_MEM;
    }
    /* The infer stack covers the ~9.5 KB of log-mel locals (nonkiact2 memo §6)
     * and on_result runs on it: do not shrink it. */
    const BaseType_t ok_pump =
        xTaskCreatePinnedToCore(pump_task, "stt32_pump", 4096, NULL, configMAX_PRIORITIES - 1, &s.pump, core_or_any(1));
    const BaseType_t ok_infer = xTaskCreatePinnedToCore(
        infer_task, "stt32_infer", 12288, NULL,
        config->infer_priority != 0 ? config->infer_priority : STT32_DEFAULT_INFER_PRIORITY, &s.infer,
        core_or_any(config->infer_core));
    if (ok_pump != pdPASS || ok_infer != pdPASS) {
        /* Tasks that did start park on run == false; ask them to exit first. */
        s.exiting = true;
        EventBits_t expect = 0;
        if (ok_pump == pdPASS) { xTaskNotifyGive(s.pump); expect |= BIT_PUMP_EXITED; }
        if (ok_infer == pdPASS) { expect |= BIT_INFER_EXITED; }
        if (expect != 0) {
            xEventGroupWaitBits(s.ev, expect, pdFALSE, pdTRUE, pdMS_TO_TICKS(STT32_DEINIT_TIMEOUT_MS));
        }
        release_resources();
        xSemaphoreGive(s_api_lock);
        return ESP_ERR_NO_MEM;
    }
    s.state = STT32_STATE_READY;
    xSemaphoreGive(s_api_lock);
    ESP_LOGI(TAG, "ready: classes=%u", (unsigned)s.class_count);
    return ESP_OK;
}

esp_err_t stt32_listen_start(const stt32_listen_options_t *options, uint32_t *session_id_out) {
    stt32_listen_options_t opt;
    memset(&opt, 0, sizeof(opt)); /* NULL options = zeroed = CONTINUOUS, the behaviour before the extra fields */
    if (options != NULL) {
        opt = *options;
    }
    const stt32_listen_mode_t mode = opt.mode;
    if (!options_valid(&opt)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_api_lock == NULL || called_from_worker()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    if (s.state != STT32_STATE_READY) {
        xSemaphoreGive(s_api_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (mode == STT32_LISTEN_WAKE && !stt32_policy_has_wake_class(s.be->semantic_ids(), s.class_count)) {
        xSemaphoreGive(s_api_lock);
        return ESP_ERR_NOT_SUPPORTED; /* the model has no wake-word class; the state stays READY */
    }
    /* Workers are parked: drop whatever was buffered while not listening. */
    s.cfg.flush_pcm(s.cfg.pcm_context);
    lock();
    if (++s.last_session_id == 0) {
        s.last_session_id = 1;
    }
    const uint32_t session_id = s.last_session_id;
    s.mode = mode;
    s.opt = opt;
    s.wake_command = false;
    s.run_session = session_id;
    s.utterance_seq = 0;
#if CONFIG_STT32_DIAG_WINDOW
    s.window_seq = 0;
#endif
    s.guard_samples = STT32_START_GUARD_SAMPLES;
    s.run = true;
    s.state = STT32_STATE_LISTENING;
    unlock();
    xTaskNotifyGive(s.pump);
    xSemaphoreGive(s_api_lock);
    if (session_id_out != NULL) {
        *session_id_out = session_id;
    }
    return ESP_OK;
}

esp_err_t stt32_listen_stop(uint32_t timeout_ms) {
    if (s_api_lock == NULL || called_from_worker()) {
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    const TickType_t start = xTaskGetTickCount();
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    lock();
    const stt32_state_t state = s.state;
    if (state == STT32_STATE_READY) {
        s.auto_session = 0; /* invalidate an auto-stopped result not yet delivered */
    } else if (state == STT32_STATE_LISTENING) {
        begin_stop_locked();
    }
    unlock();
    if (state == STT32_STATE_READY) {
        xSemaphoreTake(s.deliver_lock, portMAX_DELAY); /* wait for one in progress */
        xSemaphoreGive(s.deliver_lock);
        xSemaphoreGive(s_api_lock);
        return ESP_OK;
    }
    if (state != STT32_STATE_LISTENING) {
        xSemaphoreGive(s_api_lock);
        return ESP_ERR_INVALID_STATE;
    }

    /* No new input reaches the pipeline once the pump is parked. */
    xTaskNotifyGive(s.pump);
    const EventBits_t bits = xEventGroupWaitBits(s.ev, BITS_PARKED, pdFALSE, pdTRUE, timeout);
    esp_err_t err = ((bits & BITS_PARKED) == BITS_PARKED) ? ESP_OK : ESP_ERR_TIMEOUT;
    if (err == ESP_OK) {
        utt_msg_t m;
        while (xQueueReceive(s.utt_q, &m, 0) == pdTRUE) {
            s.utt_busy = false; /* queued utterance never reached the model */
        }
        /* A callback in progress finishes; later ones fail the session check. */
        xSemaphoreTake(s.deliver_lock, portMAX_DELAY);
        xSemaphoreGive(s.deliver_lock);
        /* Inference in progress reads the window buffer: wait until it lets go. */
        while (s.utt_busy && (xTaskGetTickCount() - start) < timeout) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (s.utt_busy) {
            err = ESP_ERR_TIMEOUT;
        }
    }
    if (err == ESP_OK) {
        s.cfg.flush_pcm(s.cfg.pcm_context);
    }
    lock();
    s.state = (err == ESP_OK) ? STT32_STATE_READY : STT32_STATE_ERROR;
    s.run_session = 0;
    unlock();
    xSemaphoreGive(s_api_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stop timed out (read_pcm or inference did not return)");
    }
    return err;
}

stt32_state_t stt32_get_state(void) {
    return s.state;
}

esp_err_t stt32_deinit(void) {
    if (s_api_lock == NULL || called_from_worker()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    if (s.state != STT32_STATE_READY) {
        xSemaphoreGive(s_api_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s.exiting = true;
    xTaskNotifyGive(s.pump);
    const EventBits_t bits =
        xEventGroupWaitBits(s.ev, BITS_EXITED, pdFALSE, pdTRUE, pdMS_TO_TICKS(STT32_DEINIT_TIMEOUT_MS));
    if ((bits & BITS_EXITED) != BITS_EXITED) {
        s.exiting = false;
        s.state = STT32_STATE_ERROR;
        xSemaphoreGive(s_api_lock);
        return ESP_ERR_TIMEOUT;
    }
    release_resources();
    xSemaphoreGive(s_api_lock);
    return ESP_OK;
}
