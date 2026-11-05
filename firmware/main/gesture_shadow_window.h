#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gesture_svm.h"
#include "motion_detector.h"

enum class NkGestureShadowSkip : uint8_t {
    None = 0,
    NoRecordWindow,
    PartialTooShort,
    ModelUnavailable,
};

enum class NkGestureShadowKind : uint8_t {
    None = 0,
    Full,
    EventPartial,
    Skipped,
};

struct NkGestureShadowOutcome {
    NkGestureShadowKind kind = NkGestureShadowKind::None;
    NkGestureShadowSkip skip = NkGestureShadowSkip::None;
    nk_motion_event_t trigger = NK_MOTION_NONE;
    uint32_t trigger_seq = 0;
    size_t prep_count = 0;
    size_t record_count = 0;
    uint32_t feature_us = 0;
    uint32_t infer_us = 0;
    uint32_t workspace_bytes = 0;
    uint32_t model_bytes = 0;
    uint32_t window_bytes = 0;
    nk::GestureSvmPrediction result = {};
};

// One fixed 200-sample preparation plus one fixed 1200-sample record window.
// This class is platform-independent so the segmentation and partial-window
// policy can be tested on the host without FreeRTOS or a sensor.
class NkGestureShadowWindow {
public:
    static constexpr size_t kPrepSamples = 200;
    static constexpr size_t kRecordSamples = 1200;
    static constexpr size_t kMinPartialRecordSamples = 200;

    NkGestureShadowWindow();

    void reset();
    bool consume(const nk::GestureSvmSample &sample, uint32_t seq,
                 NkGestureShadowOutcome *outcome);
    bool event(nk_motion_event_t trigger, uint32_t seq,
               NkGestureShadowOutcome *outcome);

    size_t prep_count() const { return prep_count_; }
    size_t record_count() const { return record_count_; }
    bool recording() const { return recording_; }

private:
    bool infer(size_t record_count, NkGestureShadowKind kind,
               nk_motion_event_t trigger, uint32_t seq,
               NkGestureShadowOutcome *outcome);
    static void clear_outcome(NkGestureShadowOutcome *outcome);

    nk::GestureSvmSample prep_[kPrepSamples] = {};
    nk::GestureSvmSample record_[kRecordSamples] = {};
    nk::GestureSvmFeatureWorkspace workspace_ = {};
    size_t prep_count_ = 0;
    size_t record_count_ = 0;
    bool recording_ = false;
};
