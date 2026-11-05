#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gesture_svm_model.h"

namespace nk {

constexpr size_t kGestureSvmFeatureCount = gesture_svm_model::kFeatureCount;
constexpr size_t kGestureSvmMaxRecordSamples = 1250;
constexpr size_t kGestureSvmSupportVectorCount = gesture_svm_model::kSupportVectorCount;
constexpr size_t kGestureSvmClassCount = gesture_svm_model::kClassCount;

// Public event order follows the firmware gesture vocabulary. The trained
// SVC stores lexicographic classes; predict() performs the explicit mapping.
enum class GestureSvmEventClass : uint8_t {
    TAP1 = 0,
    TAP2,
    TAP3,
    NOD,
    TILT_LR,
    TURN_LR,
    SPIN180,
    ROCK,
    NONE,
};

struct GestureSvmSample {
    int16_t ax;
    int16_t ay;
    int16_t az;
    int16_t gx;
    int16_t gy;
    int16_t gz;
};

// All storage is caller-owned. No feature extraction or prediction path
// allocates from the heap. The arrays are sized for the recorded ~3-second
// segments at 400 Hz; callers receive false for a longer record window.
struct GestureSvmFeatureWorkspace {
    float acc[3][kGestureSvmMaxRecordSamples];
    float gyro[3][kGestureSvmMaxRecordSamples];
    float rel_acc_mag[kGestureSvmMaxRecordSamples];
    float gravity_angle[kGestureSvmMaxRecordSamples];
    float scratch0[kGestureSvmMaxRecordSamples];
    float scratch1[kGestureSvmMaxRecordSamples];
    uint16_t peak_indices[kGestureSvmMaxRecordSamples];
    uint8_t peak_keep[kGestureSvmMaxRecordSamples];
};

constexpr size_t kGestureSvmWorkspaceBytes = sizeof(GestureSvmFeatureWorkspace);
constexpr size_t kGestureSvmModelBytes =
    sizeof(gesture_svm_model::kScalerMean) +
    sizeof(gesture_svm_model::kScalerInvScale) +
    sizeof(gesture_svm_model::kSupportVectors) +
    sizeof(gesture_svm_model::kDualCoefficients) +
    sizeof(gesture_svm_model::kIntercept);

struct GestureSvmPrediction {
    uint8_t model_class_index;
    GestureSvmEventClass event_class;
    const char *class_name;
    uint8_t votes[kGestureSvmClassCount];
    uint8_t winning_vote_margin;
    float pairwise_decision[gesture_svm_model::kPairCount];
};

// Extract the exact 276-column feature order exported in gesture_svm_model.h.
// prep_samples supplies the local gravity reference; only its last 200
// samples are used. record_samples contains the gesture segment.
bool extract_gesture_svm_features(
    const GestureSvmSample *prep_samples,
    size_t prep_count,
    const GestureSvmSample *record_samples,
    size_t record_count,
    GestureSvmFeatureWorkspace *workspace,
    float out_features[kGestureSvmFeatureCount]);

// Run the exported StandardScaler + RBF SVC. winning_vote_margin is a vote
// margin, not a calibrated probability.
bool gesture_svm_predict(
    const float features[kGestureSvmFeatureCount],
    GestureSvmPrediction *out_prediction);

}  // namespace nk
