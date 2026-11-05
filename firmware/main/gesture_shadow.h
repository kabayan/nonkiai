#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gesture_svm.h"
#include "motion_detector.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NK_SHADOW_RESET_INITIAL = 0,
    NK_SHADOW_RESET_SUSPENDED,
    NK_SHADOW_RESET_SPEECH_PENDING,
    NK_SHADOW_RESET_SPEECH_PLAYING,
    NK_SHADOW_RESET_CUE,
    NK_SHADOW_RESET_ODR_MISMATCH,
    NK_SHADOW_RESET_ODR_RECOVERED,
    NK_SHADOW_RESET_SEQUENCE_GAP,
    NK_SHADOW_RESET_TIME_GAP,
    NK_SHADOW_RESET_RING_DROP,
    NK_SHADOW_RESET_EVENT_BOUNDARY,
    NK_SHADOW_RESET_FULL_WINDOW,
};

bool nk_gesture_shadow_start_task(void);

// The sampler calls this once for every newly acquired acceleration sample.
// It never waits for the worker; a full ring increments the drop counter.
void nk_gesture_shadow_push(const nk::GestureSvmSample *sample,
                            uint32_t seq, uint32_t t_us, uint16_t odr_hz);

// Reset requests are epoch based so a pause or ODR change can invalidate
// queued samples without touching the consumer-owned read index.
void nk_gesture_shadow_reset(uint8_t reason);
void nk_gesture_shadow_rate_changed(uint16_t odr_hz);

// Called before the current event starts speech/cue handling. The event item
// is ordered after the triggering sample, so a usable partial window is still
// available even though the sampler pauses immediately afterward.
void nk_gesture_shadow_event(nk_motion_event_t event, uint32_t seq);

#ifdef __cplusplus
}
#endif
