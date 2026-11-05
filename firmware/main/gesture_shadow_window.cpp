#include "gesture_shadow_window.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
static uint64_t shadow_now_us()
{
    return static_cast<uint64_t>(esp_timer_get_time());
}
#else
#include <chrono>
static uint64_t shadow_now_us()
{
    using Clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count());
}
#endif

NkGestureShadowWindow::NkGestureShadowWindow()
{
    reset();
}

void NkGestureShadowWindow::reset()
{
    prep_count_ = 0;
    record_count_ = 0;
    recording_ = false;
}

void NkGestureShadowWindow::clear_outcome(NkGestureShadowOutcome *outcome)
{
    if (outcome != nullptr) *outcome = NkGestureShadowOutcome{};
}

bool NkGestureShadowWindow::consume(const nk::GestureSvmSample &sample,
                                    uint32_t seq,
                                    NkGestureShadowOutcome *outcome)
{
    clear_outcome(outcome);
    if (!recording_) {
        prep_[prep_count_++] = sample;
        if (prep_count_ == kPrepSamples) {
            recording_ = true;
        }
        return false;
    }

    record_[record_count_++] = sample;
    if (record_count_ < kRecordSamples) return false;
    const bool emitted = infer(kRecordSamples, NkGestureShadowKind::Full,
                               NK_MOTION_NONE, seq, outcome);
    reset();
    return emitted;
}

bool NkGestureShadowWindow::event(nk_motion_event_t trigger, uint32_t seq,
                                  NkGestureShadowOutcome *outcome)
{
    clear_outcome(outcome);
    if (!recording_) {
        if (outcome != nullptr) {
            outcome->kind = NkGestureShadowKind::Skipped;
            outcome->skip = NkGestureShadowSkip::NoRecordWindow;
            outcome->trigger = trigger;
            outcome->trigger_seq = seq;
            outcome->prep_count = prep_count_;
            outcome->record_count = record_count_;
        }
        reset();
        return false;
    }
    if (record_count_ < kMinPartialRecordSamples) {
        if (outcome != nullptr) {
            outcome->kind = NkGestureShadowKind::Skipped;
            outcome->skip = NkGestureShadowSkip::PartialTooShort;
            outcome->trigger = trigger;
            outcome->trigger_seq = seq;
            outcome->prep_count = prep_count_;
            outcome->record_count = record_count_;
        }
        reset();
        return false;
    }
    const bool emitted = infer(record_count_, NkGestureShadowKind::EventPartial,
                               trigger, seq, outcome);
    reset();
    return emitted;
}

bool NkGestureShadowWindow::infer(size_t record_count,
                                  NkGestureShadowKind kind,
                                  nk_motion_event_t trigger,
                                  uint32_t seq,
                                  NkGestureShadowOutcome *outcome)
{
    NkGestureShadowOutcome local;
    NkGestureShadowOutcome *dst = outcome != nullptr ? outcome : &local;
    clear_outcome(dst);
    dst->kind = kind;
    dst->trigger = trigger;
    dst->trigger_seq = seq;
    dst->prep_count = prep_count_;
    dst->record_count = record_count;
    dst->workspace_bytes = static_cast<uint32_t>(nk::kGestureSvmWorkspaceBytes);
    dst->model_bytes = static_cast<uint32_t>(nk::kGestureSvmModelBytes);
    dst->window_bytes = static_cast<uint32_t>(sizeof(NkGestureShadowWindow));

    float features[nk::kGestureSvmFeatureCount] = {};
    const uint64_t feature_begin = shadow_now_us();
    const bool extracted = nk::extract_gesture_svm_features(
        prep_, prep_count_, record_, record_count, &workspace_, features);
    dst->feature_us = static_cast<uint32_t>(shadow_now_us() - feature_begin);
    if (!extracted) {
        dst->kind = NkGestureShadowKind::Skipped;
        dst->skip = NkGestureShadowSkip::ModelUnavailable;
        return false;
    }

    const uint64_t infer_begin = shadow_now_us();
    const bool predicted = nk::gesture_svm_predict(features, &dst->result);
    dst->infer_us = static_cast<uint32_t>(shadow_now_us() - infer_begin);
    if (!predicted) {
        dst->kind = NkGestureShadowKind::Skipped;
        dst->skip = NkGestureShadowSkip::ModelUnavailable;
        return false;
    }
    return true;
}
