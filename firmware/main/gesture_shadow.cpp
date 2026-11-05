#include "gesture_shadow.h"

#include <atomic>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gesture_shadow_window.h"
#include "imu_sampler.h"
#include "sano_voice.h"
#include "task_stack_sizes.h"

// This generated 9-row fixture is used only for a one-time boot diagnostic.
// It exercises the deployed model and its public class mapping without
// feeding synthetic samples into the live sampler/window.
#include "../../artifacts/gesture_ml/svm_fixture_9x276.h"

static const char *const kTag = "nk-shadow";

namespace {

// A full 1200-sample feature pass currently takes about 0.63 s on the
// ESP32-S3. At 400 Hz that is roughly 256 queued samples; keep enough
// bounded headroom for the pass while the sampler remains non-blocking.
constexpr uint32_t kRingCapacity = 1024;
constexpr uint32_t kMaxSampleGapUs = 10000;

enum class ItemKind : uint8_t { Sample = 0, Event = 1 };

struct RingItem {
    ItemKind kind;
    uint8_t reason;
    uint8_t reserved[2];
    uint32_t epoch;
    uint32_t seq;
    uint32_t t_us;
    uint16_t odr_hz;
    nk_motion_event_t event;
    nk::GestureSvmSample sample;
};

RingItem *s_ring = nullptr;
std::atomic<uint32_t> s_write{0};
std::atomic<uint32_t> s_read{0};
std::atomic<uint32_t> s_epoch{1};
std::atomic<uint8_t> s_reset_reason{NK_SHADOW_RESET_INITIAL};
std::atomic<uint32_t> s_ring_drops{0};
std::atomic<bool> s_drop_invalidated{false};
std::atomic<uint32_t> s_event_pending{0};
std::atomic<bool> s_deferred_reset{false};
std::atomic<uint8_t> s_deferred_reset_reason{NK_SHADOW_RESET_EVENT_BOUNDARY};
std::atomic<bool> s_accept_input{false};
std::atomic<uint32_t> s_warmup_sample_drops{0};
std::atomic<uint32_t> s_warmup_event_drops{0};
TaskHandle_t s_task = nullptr;
NkGestureShadowWindow *s_window = nullptr;

constexpr uint32_t kRingBytes = sizeof(RingItem) * kRingCapacity;
constexpr uint32_t kWindowBytes = sizeof(NkGestureShadowWindow);
constexpr uint32_t kShadowStorageCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

void release_shadow_storage()
{
    if (s_window != nullptr) {
        s_window->~NkGestureShadowWindow();
        heap_caps_free(s_window);
        s_window = nullptr;
    }
    if (s_ring != nullptr) {
        heap_caps_free(s_ring);
        s_ring = nullptr;
    }
}

bool allocate_shadow_storage()
{
    s_accept_input.store(false, std::memory_order_release);
    s_warmup_sample_drops.store(0, std::memory_order_relaxed);
    s_warmup_event_drops.store(0, std::memory_order_relaxed);
    s_ring = static_cast<RingItem *>(
        heap_caps_calloc(kRingCapacity, sizeof(RingItem), kShadowStorageCaps));
    if (s_ring == nullptr) {
        ESP_LOGE(kTag,
                 "shadow disabled: PSRAM ring allocation failed bytes=%lu free_psram=%lu",
                 (unsigned long)kRingBytes,
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return false;
    }

    void *window_memory = heap_caps_malloc(kWindowBytes, kShadowStorageCaps);
    if (window_memory == nullptr) {
        ESP_LOGE(kTag,
                 "shadow disabled: PSRAM window allocation failed bytes=%lu free_psram=%lu",
                 (unsigned long)kWindowBytes,
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        release_shadow_storage();
        return false;
    }
    s_window = new (window_memory) NkGestureShadowWindow();
    ESP_LOGI(kTag,
             "shadow storage allocated PSRAM ring_bytes=%lu window_bytes=%lu "
             "workspace_bytes=%lu model_bytes=%lu free_psram=%lu",
             (unsigned long)kRingBytes, (unsigned long)kWindowBytes,
             (unsigned long)nk::kGestureSvmWorkspaceBytes,
             (unsigned long)nk::kGestureSvmModelBytes,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

bool ring_push(const RingItem &item)
{
    if (s_ring == nullptr) return false;
    const uint32_t write = s_write.load(std::memory_order_relaxed);
    const uint32_t read = s_read.load(std::memory_order_acquire);
    if (write - read >= kRingCapacity) {
        const uint32_t dropped = s_ring_drops.fetch_add(1, std::memory_order_relaxed) + 1;
        // A dropped sample invalidates the active fixed window immediately.
        // The consumer may be inside feature extraction, so the epoch is the
        // only non-blocking way to prevent it from accepting the stale tail
        // after the ring recovers.  Collapse one contiguous full-ring episode
        // into one reset request to keep the sampler-side path bounded.
        if (!s_drop_invalidated.exchange(true, std::memory_order_acq_rel)) {
            s_reset_reason.store(NK_SHADOW_RESET_RING_DROP, std::memory_order_release);
            s_epoch.fetch_add(1, std::memory_order_acq_rel);
            if (s_task != nullptr) xTaskNotifyGive(s_task);
        }
        if (dropped == 1 || (dropped & 63u) == 0) {
            ESP_LOGW(kTag, "[SHADOW RING DROP] total=%lu sampler_seq=%lu",
                     (unsigned long)dropped, (unsigned long)nk_imu_seq());
        }
        return false;
    }
    s_ring[write % kRingCapacity] = item;
    s_write.store(write + 1, std::memory_order_release);
    if ((write + 1) - read < kRingCapacity / 2) {
        s_drop_invalidated.store(false, std::memory_order_release);
    }
    if (s_task != nullptr) xTaskNotifyGive(s_task);
    return true;
}

bool ring_pop(RingItem *item)
{
    if (s_ring == nullptr || item == nullptr) return false;
    const uint32_t read = s_read.load(std::memory_order_relaxed);
    const uint32_t write = s_write.load(std::memory_order_acquire);
    if (read == write) return false;
    *item = s_ring[read % kRingCapacity];
    s_read.store(read + 1, std::memory_order_release);
    return true;
}

const char *reset_name(uint8_t reason)
{
    switch (reason) {
    case NK_SHADOW_RESET_SUSPENDED: return "suspended";
    case NK_SHADOW_RESET_SPEECH_PENDING: return "speech_pending";
    case NK_SHADOW_RESET_SPEECH_PLAYING: return "speech_playing";
    case NK_SHADOW_RESET_CUE: return "cue";
    case NK_SHADOW_RESET_ODR_MISMATCH: return "odr_mismatch";
    case NK_SHADOW_RESET_ODR_RECOVERED: return "odr_recovered";
    case NK_SHADOW_RESET_SEQUENCE_GAP: return "sequence_gap";
    case NK_SHADOW_RESET_TIME_GAP: return "time_gap";
    case NK_SHADOW_RESET_RING_DROP: return "ring_drop";
    case NK_SHADOW_RESET_EVENT_BOUNDARY: return "event_boundary";
    case NK_SHADOW_RESET_FULL_WINDOW: return "full_window";
    case NK_SHADOW_RESET_INITIAL:
    default: return "initial";
    }
}

const char *kind_name(NkGestureShadowKind kind)
{
    switch (kind) {
    case NkGestureShadowKind::Full: return "full";
    case NkGestureShadowKind::EventPartial: return "event_partial";
    case NkGestureShadowKind::Skipped: return "skip";
    case NkGestureShadowKind::None:
    default: return "none";
    }
}

const char *skip_name(NkGestureShadowSkip skip)
{
    switch (skip) {
    case NkGestureShadowSkip::NoRecordWindow: return "no_record_window";
    case NkGestureShadowSkip::PartialTooShort: return "partial_too_short";
    case NkGestureShadowSkip::ModelUnavailable: return "model_unavailable";
    case NkGestureShadowSkip::None:
    default: return "none";
    }
}

void log_outcome(const NkGestureShadowOutcome &outcome,
                 uint32_t sampler_seq, uint32_t ring_drops,
                 uint32_t sequence_gaps, uint32_t time_gaps,
                 uint32_t worker_busy, uint32_t ring_pending)
{
    if (outcome.kind == NkGestureShadowKind::Skipped) {
        ESP_LOGI(kTag,
                 "[SHADOW] kind=%s skip=%s trigger=%s trigger_seq=%lu prep=%u record=%u "
                 "feature_us=%lu infer_us=%lu workspace=%lu model=%lu window=%lu ring_bytes=%lu "
                 "ring_drops=%lu seq_gaps=%lu time_gaps=%lu worker_busy=%lu ring_pending=%lu "
                 "sampler_seq=%lu",
                 kind_name(outcome.kind), skip_name(outcome.skip),
                 nk_motion_event_name(outcome.trigger), (unsigned long)outcome.trigger_seq,
                 (unsigned)outcome.prep_count, (unsigned)outcome.record_count,
                 (unsigned long)outcome.feature_us, (unsigned long)outcome.infer_us,
                 (unsigned long)outcome.workspace_bytes, (unsigned long)outcome.model_bytes,
                 (unsigned long)outcome.window_bytes, (unsigned long)kRingBytes,
                 (unsigned long)ring_drops,
                 (unsigned long)sequence_gaps, (unsigned long)time_gaps,
                 (unsigned long)worker_busy, (unsigned long)ring_pending,
                 (unsigned long)sampler_seq);
        return;
    }
    ESP_LOGI(kTag,
             "[SHADOW] kind=%s pred=%s vote_margin=%u trigger=%s trigger_seq=%lu "
             "prep=%u record=%u feature_us=%lu infer_us=%lu workspace=%lu model=%lu window=%lu "
             "ring_bytes=%lu ring_drops=%lu seq_gaps=%lu time_gaps=%lu worker_busy=%lu "
             "ring_pending=%lu sampler_seq=%lu",
             kind_name(outcome.kind),
             outcome.result.class_name != nullptr ? outcome.result.class_name : "unknown",
             (unsigned)outcome.result.winning_vote_margin,
             nk_motion_event_name(outcome.trigger),
             (unsigned long)outcome.trigger_seq, (unsigned)outcome.prep_count,
             (unsigned)outcome.record_count, (unsigned long)outcome.feature_us,
             (unsigned long)outcome.infer_us, (unsigned long)outcome.workspace_bytes,
             (unsigned long)outcome.model_bytes, (unsigned long)outcome.window_bytes,
             (unsigned long)kRingBytes, (unsigned long)ring_drops,
             (unsigned long)sequence_gaps,
             (unsigned long)time_gaps, (unsigned long)worker_busy,
             (unsigned long)ring_pending, (unsigned long)sampler_seq);
}

void run_startup_fixture()
{
    bool all_pass = true;
    ESP_LOGI(kTag, "[SHADOW FIXTURE] begin classes=%u features=%u",
             (unsigned)nk::gesture_svm_fixture::kFixtureCount,
             (unsigned)nk::gesture_svm_fixture::kFeatureCount);
    for (size_t i = 0; i < nk::gesture_svm_fixture::kFixtureCount; ++i) {
        nk::GestureSvmPrediction prediction = {};
        const float *features =
            &nk::gesture_svm_fixture::kFeatures[i * nk::gesture_svm_fixture::kFeatureCount];
        const bool predicted = nk::gesture_svm_predict(features, &prediction);
        const bool pass = predicted &&
                          prediction.model_class_index ==
                              nk::gesture_svm_fixture::kExpectedClass[i];
        all_pass = all_pass && pass;
        ESP_LOGI(kTag,
                 "[SHADOW FIXTURE] idx=%u expected=%u/%s got=%u/%s event=%u ok=%u",
                 (unsigned)i,
                 (unsigned)nk::gesture_svm_fixture::kExpectedClass[i],
                 nk::gesture_svm_fixture::kClassNames[i],
                 predicted ? (unsigned)prediction.model_class_index : 255u,
                 predicted && prediction.class_name != nullptr ? prediction.class_name : "unavailable",
                 predicted ? (unsigned)prediction.event_class : 255u,
                 pass ? 1u : 0u);
    }
    ESP_LOGI(kTag, "[SHADOW FIXTURE SUMMARY] classes=%u pass=%u",
             (unsigned)nk::gesture_svm_fixture::kFixtureCount, all_pass ? 1u : 0u);
}

void shadow_task(void *)
{
    // HTS loads its model and dictionaries on Core 1. Keep the shadow path
    // gated until that one-time work completes so feature extraction cannot
    // compete with the voice startup path. Samples/events during this gate
    // are intentionally counted and discarded; no stale pre-voice window is
    // admitted after the gate opens. If voice loading is unavailable, the
    // bounded timeout preserves the shadow diagnostic and current detector.
    constexpr int64_t kVoiceWarmupTimeoutUs = 20000000;
    const int64_t warmup_deadline = esp_timer_get_time() + kVoiceWarmupTimeoutUs;
    while (!sano_voice_is_ready() && esp_timer_get_time() < warmup_deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    const bool voice_ready = sano_voice_is_ready();
    const uint32_t warmup_sample_drops =
        s_warmup_sample_drops.exchange(0, std::memory_order_acq_rel);
    const uint32_t warmup_event_drops =
        s_warmup_event_drops.exchange(0, std::memory_order_acq_rel);
    s_accept_input.store(true, std::memory_order_release);
    ESP_LOGI(kTag,
             "[SHADOW WARMUP] voice_ready=%u gated_samples=%lu gated_events=%lu timeout_ms=%lu",
             voice_ready ? 1u : 0u, (unsigned long)warmup_sample_drops,
             (unsigned long)warmup_event_drops,
             (unsigned long)(kVoiceWarmupTimeoutUs / 1000));
    if (!voice_ready) {
        ESP_LOGW(kTag, "[SHADOW WARMUP] HTS not ready after timeout; continuing shadow input");
    }

    // Run on the low-priority shadow worker so the sampler and audio paths
    // remain independent while the deployed model's nine class mappings are
    // checked exactly once per boot.
    run_startup_fixture();

    uint32_t local_epoch = s_epoch.load(std::memory_order_acquire);
    uint32_t previous_seq = 0;
    uint32_t previous_t_us = 0;
    bool have_previous = false;
    uint32_t sequence_gaps = 0;
    uint32_t time_gaps = 0;
    uint32_t worker_busy = 0;
    bool odr_disabled = false;

    auto apply_epoch_reset = [&](uint32_t epoch, uint8_t reason) {
        local_epoch = epoch;
        s_window->reset();
        have_previous = false;
        odr_disabled = false;
        ESP_LOGI(kTag, "[SHADOW RESET] reason=%s epoch=%lu sampler_seq=%lu",
                 reset_name(reason), (unsigned long)local_epoch,
                 (unsigned long)nk_imu_seq());
    };

    for (;;) {
        RingItem item;
        if (!ring_pop(&item)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }

        // A reset can advance the global epoch while the ring is empty. In
        // that case the first new-epoch sample must reset the consumer before
        // it is admitted, even when no stale ring item was available to do it.
        const uint32_t observed_epoch = s_epoch.load(std::memory_order_acquire);
        if (local_epoch != observed_epoch) {
            apply_epoch_reset(observed_epoch,
                              s_reset_reason.load(std::memory_order_relaxed));
        }

        if (item.kind == ItemKind::Event) {
            NkGestureShadowOutcome outcome;
            // A pause reset is deferred while an event item is in flight, so
            // the event can still classify the usable partial window.  The
            // global epoch check closes the opposite race: an old event that
            // was queued before an unrelated reset is never applied to the
            // new window.
            const uint32_t current_epoch = s_epoch.load(std::memory_order_acquire);
            if (item.epoch == local_epoch && item.epoch == current_epoch) {
                if (s_window->event(item.event, item.seq, &outcome) ||
                    outcome.kind == NkGestureShadowKind::Skipped) {
                    log_outcome(outcome, nk_imu_seq(), s_ring_drops.load(),
                                sequence_gaps, time_gaps, worker_busy,
                                s_write.load() - s_read.load());
                }
            } else {
                ESP_LOGI(kTag, "[SHADOW EVENT SKIP] event=%s seq=%lu epoch=%lu current_epoch=%lu",
                         nk_motion_event_name(item.event), (unsigned long)item.seq,
                         (unsigned long)item.epoch, (unsigned long)local_epoch);
            }
            const uint32_t pending_events =
                s_event_pending.fetch_sub(1, std::memory_order_acq_rel);
            if (pending_events == 1 && s_deferred_reset.exchange(false, std::memory_order_acq_rel)) {
                const uint8_t reason = s_deferred_reset_reason.load(std::memory_order_acquire);
                s_reset_reason.store(reason, std::memory_order_release);
                const uint32_t epoch = s_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
                apply_epoch_reset(epoch, reason);
            }
            continue;
        }

        const uint32_t current_epoch = s_epoch.load(std::memory_order_acquire);
        if (item.epoch != current_epoch) {
            // Several stale items can remain queued after one epoch bump.
            // Reset the consumer state once, then discard the rest silently.
            if (local_epoch != current_epoch) {
                apply_epoch_reset(current_epoch, s_reset_reason.load(std::memory_order_relaxed));
            }
            continue;
        }

        if (item.odr_hz != 400) {
            if (!odr_disabled) {
                odr_disabled = true;
                s_window->reset();
                have_previous = false;
                ESP_LOGW(kTag, "[SHADOW RESET] reason=odr_mismatch odr=%u expected=400 sampler_seq=%lu",
                         (unsigned)item.odr_hz, (unsigned long)nk_imu_seq());
            }
            continue;
        }
        if (odr_disabled) {
            odr_disabled = false;
            s_window->reset();
            have_previous = false;
            ESP_LOGI(kTag, "[SHADOW RESET] reason=odr_recovered odr=400 sampler_seq=%lu",
                     (unsigned long)nk_imu_seq());
        }

        if (have_previous) {
            if (item.seq != previous_seq + 1) {
                ++sequence_gaps;
                s_window->reset();
                have_previous = false;
                ESP_LOGW(kTag, "[SHADOW RESET] reason=sequence_gap previous=%lu current=%lu sampler_seq=%lu",
                         (unsigned long)previous_seq, (unsigned long)item.seq,
                         (unsigned long)nk_imu_seq());
            } else if ((uint32_t)(item.t_us - previous_t_us) > kMaxSampleGapUs) {
                ++time_gaps;
                s_window->reset();
                have_previous = false;
                ESP_LOGW(kTag, "[SHADOW RESET] reason=time_gap delta_us=%lu sampler_seq=%lu",
                         (unsigned long)(uint32_t)(item.t_us - previous_t_us),
                         (unsigned long)nk_imu_seq());
            }
        }
        previous_seq = item.seq;
        previous_t_us = item.t_us;
        have_previous = true;

        NkGestureShadowOutcome outcome;
        const bool emitted = s_window->consume(item.sample, item.seq, &outcome);
        if (emitted || outcome.kind == NkGestureShadowKind::Skipped) {
            const uint32_t pending = s_write.load() - s_read.load();
            if (pending != 0) ++worker_busy;
            log_outcome(outcome, nk_imu_seq(), s_ring_drops.load(),
                        sequence_gaps, time_gaps, worker_busy, pending);
        }
    }
}

} // namespace

bool nk_gesture_shadow_start_task(void)
{
    if (s_task != nullptr) return true;
    if (!allocate_shadow_storage()) return false;
    const BaseType_t created = xTaskCreatePinnedToCore(
        // Keep feature extraction away from the 400 Hz sampler (Core 1) and
        // below the voice/BLE work on Core 0.  The model path is shadow-only;
        // starvation is preferable to delaying acquisition or audio.
        shadow_task, "nk_shadow", nk::kShadowStackBytes, nullptr, 1, &s_task, 0);
    if (created != pdPASS) {
        s_task = nullptr;
        release_shadow_storage();
        ESP_LOGE(kTag, "shadow worker creation failed");
        return false;
    }
    ESP_LOGI(kTag, "shadow worker started prep=%u record=%u ring=%u model_features=%u",
             (unsigned)NkGestureShadowWindow::kPrepSamples,
             (unsigned)NkGestureShadowWindow::kRecordSamples,
             (unsigned)kRingCapacity, (unsigned)nk::kGestureSvmFeatureCount);
    return true;
}

void nk_gesture_shadow_push(const nk::GestureSvmSample *sample,
                            uint32_t seq, uint32_t t_us, uint16_t odr_hz)
{
    if (sample == nullptr) return;
    if (!s_accept_input.load(std::memory_order_acquire)) {
        s_warmup_sample_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    RingItem item = {};
    item.kind = ItemKind::Sample;
    item.epoch = s_epoch.load(std::memory_order_acquire);
    item.seq = seq;
    item.t_us = t_us;
    item.odr_hz = odr_hz;
    item.sample = *sample;
    (void)ring_push(item);
}

void nk_gesture_shadow_reset(uint8_t reason)
{
    // Motion events are queued immediately before speech/cue handling.  The
    // sampler then requests a pause reset in the next loop, so defer that
    // reset until the ordered event item has been consumed.  This preserves a
    // valid event-partial inference without admitting an old-epoch event after
    // an unrelated reset.
    if (s_event_pending.load(std::memory_order_acquire) != 0) {
        s_deferred_reset_reason.store(reason, std::memory_order_release);
        s_deferred_reset.store(true, std::memory_order_release);
        if (s_task != nullptr) xTaskNotifyGive(s_task);
        return;
    }
    s_reset_reason.store(reason, std::memory_order_release);
    s_epoch.fetch_add(1, std::memory_order_acq_rel);
    if (s_task != nullptr) xTaskNotifyGive(s_task);
}

void nk_gesture_shadow_rate_changed(uint16_t odr_hz)
{
    nk_gesture_shadow_reset(odr_hz == 400 ? NK_SHADOW_RESET_ODR_RECOVERED
                                          : NK_SHADOW_RESET_ODR_MISMATCH);
}

void nk_gesture_shadow_event(nk_motion_event_t event, uint32_t seq)
{
    if (!s_accept_input.load(std::memory_order_acquire)) {
        s_warmup_event_drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    RingItem item = {};
    item.kind = ItemKind::Event;
    item.epoch = s_epoch.load(std::memory_order_acquire);
    item.seq = seq;
    item.event = event;
    s_event_pending.fetch_add(1, std::memory_order_acq_rel);
    if (!ring_push(item)) {
        // The event cannot be recovered once the bounded ring is full.  Drop
        // the pending token and invalidate the partial window explicitly.
        s_event_pending.fetch_sub(1, std::memory_order_acq_rel);
        s_deferred_reset.store(false, std::memory_order_release);
        nk_gesture_shadow_reset(NK_SHADOW_RESET_EVENT_BOUNDARY);
    }
}
