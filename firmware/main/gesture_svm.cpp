#include "gesture_svm.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace nk {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kSampleHz = 400.0;
constexpr double kDt = 1.0 / kSampleHz;
constexpr double kAccelRes = 0.000244140625;
constexpr double kGyroRes = 0.0610351562;

static inline void gesture_svm_worker_yield()
{
#ifdef ESP_PLATFORM
    // Periodicity is intentionally bounded but still scans thousands of
    // products.  Give the idle/audio tasks a tick during the scan so a
    // low-priority shadow worker cannot trip the task watchdog.
    vTaskDelay(1);
#endif
}

struct FeatureWriter {
    float *out;
    bool seen[kGestureSvmFeatureCount];
    size_t count;

    explicit FeatureWriter(float *values) : out(values), seen{}, count(0) {}

    bool set(const char *name, float value)
    {
        if (!isfinite(value)) value = 0.0f;
        for (size_t i = 0; i < kGestureSvmFeatureCount; ++i) {
            if (strcmp(name, gesture_svm_model::kFeatureNames[i]) == 0) {
                if (!seen[i]) {
                    seen[i] = true;
                    ++count;
                }
                out[i] = value;
                return true;
            }
        }
        return false;
    }

    bool complete() const { return count == kGestureSvmFeatureCount; }
};

struct Stats {
    double mean;
    double stddev;
    double min;
    double max;
    double absmax;
    double q10;
    double q25;
    double median;
    double q75;
    double q90;
    double rms;
};

static float finite_float(double value)
{
    return isfinite(value) ? static_cast<float>(value) : 0.0f;
}

static double percentile_sorted(const float *values, size_t count, double q)
{
    if (count == 0) return 0.0;
    if (count == 1) return values[0];
    const double rank = q * static_cast<double>(count - 1);
    const size_t low = static_cast<size_t>(floor(rank));
    const size_t high = low + (low + 1 < count ? 1 : 0);
    const double fraction = rank - static_cast<double>(low);
    return static_cast<double>(values[low]) +
           fraction * (static_cast<double>(values[high]) - values[low]);
}

static Stats calculate_stats(const float *values, size_t count, float *sort_buffer)
{
    Stats stats{};
    if (count == 0) return stats;
    double sum = 0.0;
    double square_sum = 0.0;
    stats.min = values[0];
    stats.max = values[0];
    stats.absmax = fabs(static_cast<double>(values[0]));
    for (size_t i = 0; i < count; ++i) {
        const double value = values[i];
        sum += value;
        square_sum += value * value;
        if (value < stats.min) stats.min = value;
        if (value > stats.max) stats.max = value;
        const double absolute = fabs(value);
        if (absolute > stats.absmax) stats.absmax = absolute;
        sort_buffer[i] = values[i];
    }
    // Only the sorted order is part of the feature contract.  Keep the
    // caller-owned bounded buffer, but use introsort so the many 1200-point
    // percentile series do not spend most of a shadow window in O(N^2)
    // insertion-sort comparisons.
    std::sort(sort_buffer, sort_buffer + count);
    stats.mean = sum / static_cast<double>(count);
    double variance_sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double delta = static_cast<double>(values[i]) - stats.mean;
        variance_sum += delta * delta;
    }
    stats.stddev = sqrt(variance_sum / static_cast<double>(count));
    stats.q10 = percentile_sorted(sort_buffer, count, 0.10);
    stats.q25 = percentile_sorted(sort_buffer, count, 0.25);
    stats.median = percentile_sorted(sort_buffer, count, 0.50);
    stats.q75 = percentile_sorted(sort_buffer, count, 0.75);
    stats.q90 = percentile_sorted(sort_buffer, count, 0.90);
    stats.rms = sqrt(square_sum / static_cast<double>(count));
    return stats;
}

static bool set_named_stats(
    FeatureWriter &writer,
    const char *prefix,
    char axis,
    const float *values,
    size_t count,
    float *sort_buffer)
{
    Stats stats = calculate_stats(values, count, sort_buffer);
    const char *suffixes[] = {
        "mean", "std", "min", "max", "absmax", "q10", "q25",
        "median", "q75", "q90", "rms",
    };
    const double outputs[] = {
        stats.mean, stats.stddev, stats.min, stats.max, stats.absmax,
        stats.q10, stats.q25, stats.median, stats.q75, stats.q90, stats.rms,
    };
    char name[96];
    for (size_t i = 0; i < 11; ++i) {
        snprintf(name, sizeof(name), "%s_%c_%s", prefix, axis, suffixes[i]);
        if (!writer.set(name, finite_float(outputs[i]))) return false;
    }
    return true;
}

static bool set_named_scalar_stats(
    FeatureWriter &writer,
    const char *prefix,
    const float *values,
    size_t count,
    float *sort_buffer)
{
    Stats stats = calculate_stats(values, count, sort_buffer);
    const char *suffixes[] = {
        "mean", "std", "min", "max", "absmax", "q10", "q25",
        "median", "q75", "q90", "rms",
    };
    const double outputs[] = {
        stats.mean, stats.stddev, stats.min, stats.max, stats.absmax,
        stats.q10, stats.q25, stats.median, stats.q75, stats.q90, stats.rms,
    };
    char name[96];
    for (size_t i = 0; i < 11; ++i) {
        snprintf(name, sizeof(name), "%s_%s", prefix, suffixes[i]);
        if (!writer.set(name, finite_float(outputs[i]))) return false;
    }
    return true;
}

static double peak_prominence(const float *values, size_t count, size_t peak)
{
    const double height = values[peak];
    double left_min = height;
    for (size_t i = peak; i > 0; --i) {
        const double value = values[i - 1];
        if (value > height) break;
        if (value < left_min) left_min = value;
    }
    double right_min = height;
    for (size_t i = peak + 1; i < count; ++i) {
        const double value = values[i];
        if (value > height) break;
        if (value < right_min) right_min = value;
    }
    const double base = left_min > right_min ? left_min : right_min;
    return height - base;
}

static size_t select_peaks(
    const float *values,
    size_t count,
    float height,
    size_t distance,
    GestureSvmFeatureWorkspace *workspace,
    double *mean_prominence)
{
    size_t candidate_count = 0;
    for (size_t i = 1; i + 1 < count; ++i) {
        // scipy.signal.find_peaks treats a flat-top maximum as one peak at
        // the midpoint (rounded down).  IMU values are quantized, so these
        // plateaus occur often enough that strict-neighbor checks change the
        // exported feature columns.
        const size_t plateau_start = i;
        const float peak_value = values[i];
        size_t plateau_end = i;
        while (plateau_end + 1 < count && values[plateau_end + 1] == peak_value) {
            ++plateau_end;
        }
        i = plateau_end;
        if (peak_value >= height && plateau_start > 0 && plateau_end + 1 < count &&
            peak_value > values[plateau_start - 1] &&
            peak_value > values[plateau_end + 1]) {
            const size_t peak = plateau_start + (plateau_end - plateau_start) / 2;
            workspace->peak_indices[candidate_count] = static_cast<uint16_t>(peak);
            workspace->peak_keep[candidate_count] = 1;
            ++candidate_count;
        }
    }
    size_t selected_count = 0;
    double prominence_sum = 0.0;
    const size_t min_distance = distance > 0 ? distance : 1;
    while (true) {
        size_t best = candidate_count;
        for (size_t i = 0; i < candidate_count; ++i) {
            if (!workspace->peak_keep[i]) continue;
            if (best == candidate_count ||
                values[workspace->peak_indices[i]] > values[workspace->peak_indices[best]] ||
                (values[workspace->peak_indices[i]] == values[workspace->peak_indices[best]] &&
                 workspace->peak_indices[i] > workspace->peak_indices[best])) {
                best = i;
            }
        }
        if (best == candidate_count) break;
        workspace->peak_keep[best] = 2;
        ++selected_count;
        if (mean_prominence) {
            prominence_sum += peak_prominence(values, count, workspace->peak_indices[best]);
        }
        const int best_index = workspace->peak_indices[best];
        for (size_t i = 0; i < candidate_count; ++i) {
            if (!workspace->peak_keep[i]) continue;
            const int distance_from_best = static_cast<int>(workspace->peak_indices[i]) - best_index;
            if (distance_from_best > -static_cast<int>(min_distance) &&
                distance_from_best < static_cast<int>(min_distance)) {
                workspace->peak_keep[i] = 0;
            }
        }
    }
    if (mean_prominence) {
        *mean_prominence = selected_count ? prominence_sum / selected_count : 0.0;
    }
    return selected_count;
}

struct PeakSummary {
    int count75;
    int count90;
    float max;
    float mean_prominence;
};

static PeakSummary calculate_peak_summary(
    const float *values,
    size_t count,
    GestureSvmFeatureWorkspace *workspace,
    float *sort_buffer)
{
    PeakSummary summary{};
    if (count == 0) return summary;
    summary.max = values[0];
    for (size_t i = 1; i < count; ++i) if (values[i] > summary.max) summary.max = values[i];
    Stats stats = calculate_stats(values, count, sort_buffer);
    double prominence = 0.0;
    summary.count75 = static_cast<int>(select_peaks(values, count, finite_float(stats.q75), 8, workspace, &prominence));
    summary.count90 = static_cast<int>(select_peaks(values, count, finite_float(stats.q90), 8, workspace, nullptr));
    summary.mean_prominence = finite_float(prominence);
    return summary;
}

struct Periodicity {
    float dominant_hz;
    float dominant_power;
    float spectral_entropy;
    float acf_peak;
    float acf_lag_s;
};

static Periodicity calculate_periodicity(const float *values, size_t count, float *normalized)
{
    Periodicity result{};
    if (count < 32 || normalized == nullptr) return result;
    // The ESP32-S3 has a single-precision FPU; keeping this hot path in
    // double makes the O(N*lag) autocorrelation monopolize the sampler core
    // for seconds.  The feature output is float32 and the host parity
    // contract already compares the exported float32 values, so float
    // accumulation is the appropriate bounded device representation here.
    float mean = 0.0f;
    for (size_t i = 0; i < count; ++i) mean += values[i];
    mean /= static_cast<float>(count);
    float variance = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const float delta = values[i] - mean;
        variance += delta * delta;
    }
    const float scale = sqrtf(variance / static_cast<float>(count));
    if (scale >= 1e-9f) {
        // Normalize once.  Recomputing both divisions inside every ACF pair
        // still made the O(N*lag) loop expensive on the ESP32-S3 even after
        // changing its accumulators to float.  This buffer is caller-owned
        // scratch storage and is overwritten for each periodicity series.
        for (size_t i = 0; i < count; ++i) {
            normalized[i] = (values[i] - mean) / scale;
        }
        const int first_bin = static_cast<int>(ceilf(0.4f * static_cast<float>(count) /
                                                      static_cast<float>(kSampleHz)));
        const int last_bin = static_cast<int>(floorf(12.0f * static_cast<float>(count) /
                                                     static_cast<float>(kSampleHz)));
        float powers[64] = {};
        int power_count = 0;
        int best_bin = first_bin;
        float best_power = -1.0f;
        float total_power = 0.0f;
        for (int bin = first_bin; bin <= last_bin && power_count < 64; ++bin) {
            const float omega = 2.0f * static_cast<float>(kPi) * static_cast<float>(bin) /
                                static_cast<float>(count);
            const float cos_step = cosf(omega);
            const float sin_step = sinf(omega);
            float cosine = 1.0f;
            float sine = 0.0f;
            float real = 0.0f;
            float imag = 0.0f;
            for (size_t i = 0; i < count; ++i) {
                const float centered = normalized[i];
                real += centered * cosine;
                imag -= centered * sine;
                const float next_cosine = cosine * cos_step - sine * sin_step;
                sine = sine * cos_step + cosine * sin_step;
                cosine = next_cosine;
            }
            const float power = real * real + imag * imag;
            powers[power_count++] = power;
            total_power += power;
            if (power > best_power) {
                best_power = power;
                best_bin = bin;
            }
            if ((bin & 7) == 7) gesture_svm_worker_yield();
        }
        if (power_count > 0 && total_power > 1e-12) {
            float entropy = 0.0f;
            for (int i = 0; i < power_count; ++i) {
                const float probability = powers[i] / total_power;
                if (probability > 0.0f) entropy -= probability * logf(probability);
            }
            result.dominant_hz = best_bin * static_cast<float>(kSampleHz) /
                                 static_cast<float>(count);
            result.dominant_power = best_power / total_power;
            result.spectral_entropy = entropy;
        }

        const size_t max_lag = count / 2 < 800 ? count / 2 : 800;
        const size_t lag_lo = 40;
        if (max_lag > lag_lo + 1) {
            float acf_zero = 0.0f;
            for (size_t i = 0; i < count; ++i) {
                const float centered = normalized[i];
                acf_zero += centered * centered;
            }
            float best_acf = -1e30f;
            size_t best_lag = lag_lo;
            for (size_t lag = lag_lo; lag < max_lag; ++lag) {
                float sum = 0.0f;
                for (size_t i = 0; i + lag < count; ++i) {
                    sum += normalized[i] * normalized[i + lag];
                }
                const float acf = sum / (acf_zero + 1e-12f);
                if (acf > best_acf) {
                    best_acf = acf;
                    best_lag = lag;
                }
                if ((lag & 31u) == 31u) gesture_svm_worker_yield();
            }
            result.acf_peak = best_acf;
            result.acf_lag_s = static_cast<float>(best_lag) / static_cast<float>(kSampleHz);
        }
    }
    return result;
}

static bool set_periodicity(FeatureWriter &writer, const char *prefix, const Periodicity &period)
{
    char name[96];
    const char *suffixes[] = {
        "dominant_hz", "dominant_power", "spectral_entropy", "acf_peak", "acf_lag_s",
    };
    const float values[] = {
        period.dominant_hz, period.dominant_power, period.spectral_entropy,
        period.acf_peak, period.acf_lag_s,
    };
    for (size_t i = 0; i < 5; ++i) {
        snprintf(name, sizeof(name), "%s_%s", prefix, suffixes[i]);
        if (!writer.set(name, values[i])) return false;
    }
    return true;
}

static double vector_norm(const double *values)
{
    return sqrt(values[0] * values[0] + values[1] * values[1] + values[2] * values[2]);
}

static void set_zero_prediction(GestureSvmPrediction *prediction)
{
    memset(prediction, 0, sizeof(*prediction));
    prediction->event_class = GestureSvmEventClass::NONE;
}

}  // namespace

bool extract_gesture_svm_features(
    const GestureSvmSample *prep_samples,
    size_t prep_count,
    const GestureSvmSample *record_samples,
    size_t record_count,
    GestureSvmFeatureWorkspace *workspace,
    float out_features[kGestureSvmFeatureCount])
{
    if (!prep_samples || !record_samples || !workspace || !out_features ||
        prep_count == 0 || record_count == 0 || record_count > kGestureSvmMaxRecordSamples) {
        return false;
    }
    memset(out_features, 0, sizeof(float) * kGestureSvmFeatureCount);
    const size_t prep_start = prep_count > 200 ? prep_count - 200 : 0;
    const size_t prep_used = prep_count - prep_start;
    double base[3] = {};
    for (size_t i = prep_start; i < prep_count; ++i) {
        base[0] += static_cast<double>(prep_samples[i].ax) * kAccelRes;
        base[1] += static_cast<double>(prep_samples[i].ay) * kAccelRes;
        base[2] += static_cast<double>(prep_samples[i].az) * kAccelRes;
    }
    for (double &value : base) value /= prep_used;
    const double base_norm = vector_norm(base);
    double base_unit[3] = {0.0, 0.0, 1.0};
    if (base_norm > 1e-9) {
        base_unit[0] = base[0] / base_norm;
        base_unit[1] = base[1] / base_norm;
        base_unit[2] = base[2] / base_norm;
    }

    double ref[3] = {0.0, 0.0, 1.0};
    if (fabs(base_unit[2]) > 0.9) {
        ref[0] = 0.0;
        ref[1] = 1.0;
        ref[2] = 0.0;
    }
    double lateral_1[3] = {
        base_unit[1] * ref[2] - base_unit[2] * ref[1],
        base_unit[2] * ref[0] - base_unit[0] * ref[2],
        base_unit[0] * ref[1] - base_unit[1] * ref[0],
    };
    double lateral_1_norm = vector_norm(lateral_1);
    if (lateral_1_norm < 1e-9) lateral_1_norm = 1.0;
    for (double &value : lateral_1) value /= lateral_1_norm;
    double lateral_2[3] = {
        base_unit[1] * lateral_1[2] - base_unit[2] * lateral_1[1],
        base_unit[2] * lateral_1[0] - base_unit[0] * lateral_1[2],
        base_unit[0] * lateral_1[1] - base_unit[1] * lateral_1[0],
    };
    double lateral_2_norm = vector_norm(lateral_2);
    if (lateral_2_norm < 1e-9) lateral_2_norm = 1.0;
    for (double &value : lateral_2) value /= lateral_2_norm;

    for (size_t i = 0; i < record_count; ++i) {
        const GestureSvmSample &sample = record_samples[i];
        const double acc[3] = {
            static_cast<double>(sample.ax) * kAccelRes,
            static_cast<double>(sample.ay) * kAccelRes,
            static_cast<double>(sample.az) * kAccelRes,
        };
        const double gyro[3] = {
            static_cast<double>(sample.gx) * kGyroRes,
            static_cast<double>(sample.gy) * kGyroRes,
            static_cast<double>(sample.gz) * kGyroRes,
        };
        for (size_t axis = 0; axis < 3; ++axis) {
            workspace->acc[axis][i] = static_cast<float>(acc[axis]);
            workspace->gyro[axis][i] = static_cast<float>(gyro[axis]);
        }
        const double relative[3] = {
            acc[0] - base[0], acc[1] - base[1], acc[2] - base[2],
        };
        workspace->rel_acc_mag[i] = finite_float(vector_norm(relative));
        const double acc_norm = vector_norm(acc);
        double dot = (acc[0] * base_unit[0] + acc[1] * base_unit[1] + acc[2] * base_unit[2]) /
                     (acc_norm + 1e-9);
        if (dot < -1.0) dot = -1.0;
        if (dot > 1.0) dot = 1.0;
        workspace->gravity_angle[i] = finite_float(acos(dot) * (180.0 / kPi));
    }

    FeatureWriter writer(out_features);
    // Axis statistics.
    for (size_t axis = 0; axis < 3; ++axis) {
        if (!set_named_stats(writer, "acc", static_cast<char>('x' + axis),
                             workspace->acc[axis], record_count, workspace->scratch0)) return false;
        for (size_t i = 0; i < record_count; ++i) {
            workspace->scratch0[i] = workspace->acc[axis][i] - static_cast<float>(base[axis]);
        }
        if (!set_named_stats(writer, "rel_acc", static_cast<char>('x' + axis),
                             workspace->scratch0, record_count, workspace->scratch1)) return false;
        if (!set_named_stats(writer, "gyro", static_cast<char>('x' + axis),
                             workspace->gyro[axis], record_count, workspace->scratch0)) return false;
    }
    // Gravity-aligned gyro frame statistics.
    for (size_t axis = 0; axis < 3; ++axis) {
        for (size_t i = 0; i < record_count; ++i) {
            const double gx = workspace->gyro[0][i];
            const double gy = workspace->gyro[1][i];
            const double gz = workspace->gyro[2][i];
            const double frame_axis = axis == 0
                ? gx * lateral_1[0] + gy * lateral_1[1] + gz * lateral_1[2]
                : axis == 1
                    ? gx * lateral_2[0] + gy * lateral_2[1] + gz * lateral_2[2]
                    : gx * base_unit[0] + gy * base_unit[1] + gz * base_unit[2];
            workspace->scratch0[i] = finite_float(frame_axis);
        }
        if (!set_named_stats(writer, "gyro_frame", static_cast<char>('x' + axis),
                             workspace->scratch0, record_count, workspace->scratch1)) return false;
    }

    // Scalar statistics.
    for (size_t i = 0; i < record_count; ++i) {
        const double ax = workspace->acc[0][i];
        const double ay = workspace->acc[1][i];
        const double az = workspace->acc[2][i];
        workspace->scratch0[i] = finite_float(sqrt(ax * ax + ay * ay + az * az));
    }
    if (!set_named_scalar_stats(writer, "acc_mag", workspace->scratch0, record_count, workspace->scratch1)) return false;
    if (!set_named_scalar_stats(writer, "rel_acc_mag", workspace->rel_acc_mag, record_count, workspace->scratch0)) return false;
    for (size_t i = 0; i < record_count; ++i) {
        const double gx = workspace->gyro[0][i];
        const double gy = workspace->gyro[1][i];
        const double gz = workspace->gyro[2][i];
        workspace->scratch0[i] = finite_float(sqrt(gx * gx + gy * gy + gz * gz));
    }
    if (!set_named_scalar_stats(writer, "gyro_mag", workspace->scratch0, record_count, workspace->scratch1)) return false;
    if (!set_named_scalar_stats(writer, "gravity_angle", workspace->gravity_angle, record_count, workspace->scratch0)) return false;
    for (size_t i = 0; i < record_count; ++i) {
        const double gx = workspace->gyro[0][i];
        const double gy = workspace->gyro[1][i];
        const double gz = workspace->gyro[2][i];
        workspace->scratch0[i] = finite_float(gx * base_unit[0] + gy * base_unit[1] + gz * base_unit[2]);
    }
    if (!set_named_scalar_stats(writer, "gyro_projection", workspace->scratch0, record_count, workspace->scratch1)) return false;

    // Aggregate posture, energy, and threshold features.
    double dynamic_mean[3] = {};
    for (size_t i = 0; i < record_count; ++i) for (size_t axis = 0; axis < 3; ++axis) {
        dynamic_mean[axis] += static_cast<double>(workspace->acc[axis][i]) - base[axis];
    }
    for (double &value : dynamic_mean) value /= record_count;
    double acc_dynamic_energy = 0.0;
    double acc_dynamic_axis_energy[3] = {};
    double gyro_energy = 0.0;
    double gyro_axis_energy[3] = {};
    double gyro_projection_energy = 0.0;
    double gyro_lateral_energy = 0.0;
    double gyro_integral[3] = {};
    double gyro_abs_integral[3] = {};
    double frame_integral[3] = {};
    double cumulative[3] = {};
    double cumulative_frame[3] = {};
    double cumulative_absmax[3] = {};
    double cumulative_frame_absmax[3] = {};
    double max_angle = 0.0;
    double sum_angle = 0.0;
    size_t angle_gt20 = 0, angle_gt45 = 0, angle_gt60 = 0, angle_gt90 = 0;
    for (size_t i = 0; i < record_count; ++i) {
        double relative[3] = {};
        for (size_t axis = 0; axis < 3; ++axis) {
            relative[axis] = static_cast<double>(workspace->acc[axis][i]) - base[axis] - dynamic_mean[axis];
            acc_dynamic_axis_energy[axis] += relative[axis] * relative[axis];
            const double g = workspace->gyro[axis][i];
            gyro_axis_energy[axis] += g * g;
            gyro_integral[axis] += g * kDt;
            gyro_abs_integral[axis] += fabs(g) * kDt;
            cumulative[axis] += g * kDt;
            const double absolute = fabs(cumulative[axis]);
            if (absolute > cumulative_absmax[axis]) cumulative_absmax[axis] = absolute;
        }
        acc_dynamic_energy += relative[0] * relative[0] + relative[1] * relative[1] + relative[2] * relative[2];
        const double gx = workspace->gyro[0][i];
        const double gy = workspace->gyro[1][i];
        const double gz = workspace->gyro[2][i];
        const double frame[3] = {
            gx * lateral_1[0] + gy * lateral_1[1] + gz * lateral_1[2],
            gx * lateral_2[0] + gy * lateral_2[1] + gz * lateral_2[2],
            gx * base_unit[0] + gy * base_unit[1] + gz * base_unit[2],
        };
        for (size_t axis = 0; axis < 3; ++axis) {
            frame_integral[axis] += frame[axis] * kDt;
            cumulative_frame[axis] += frame[axis] * kDt;
            const double absolute = fabs(cumulative_frame[axis]);
            if (absolute > cumulative_frame_absmax[axis]) cumulative_frame_absmax[axis] = absolute;
        }
        gyro_energy += gx * gx + gy * gy + gz * gz;
        gyro_projection_energy += frame[2] * frame[2];
        gyro_lateral_energy += frame[0] * frame[0] + frame[1] * frame[1];
        const double angle = workspace->gravity_angle[i];
        if (angle > max_angle) max_angle = angle;
        sum_angle += angle;
        if (angle > 20.0) ++angle_gt20;
        if (angle > 45.0) ++angle_gt45;
        if (angle > 60.0) ++angle_gt60;
        if (angle > 90.0) ++angle_gt90;
    }
    writer.set("record_final_angle", workspace->gravity_angle[record_count - 1]);
    writer.set("record_max_angle", finite_float(max_angle));
    writer.set("record_mean_angle", finite_float(sum_angle / record_count));
    writer.set("record_angle_gt_20_frac", finite_float(static_cast<double>(angle_gt20) / record_count));
    writer.set("record_angle_gt_45_frac", finite_float(static_cast<double>(angle_gt45) / record_count));
    writer.set("record_angle_gt_60_frac", finite_float(static_cast<double>(angle_gt60) / record_count));
    writer.set("record_angle_gt_90_frac", finite_float(static_cast<double>(angle_gt90) / record_count));
    // Python training features average the squared dynamic acceleration over
    // both samples and the three axes (`np.mean(dynamic_acc * dynamic_acc)`).
    // Keep the aggregate feature on that same scale; the per-axis features
    // below remain sample means.
    writer.set("acc_dynamic_energy", finite_float(acc_dynamic_energy / (record_count * 3.0)));
    for (size_t axis = 0; axis < 3; ++axis) {
        char name[48];
        snprintf(name, sizeof(name), "acc_dynamic_energy_%c", static_cast<char>('x' + axis));
        writer.set(name, finite_float(acc_dynamic_axis_energy[axis] / record_count));
        snprintf(name, sizeof(name), "gyro_energy_%c", static_cast<char>('x' + axis));
        writer.set(name, finite_float(gyro_axis_energy[axis] / record_count));
    }
    writer.set("gyro_energy", finite_float(gyro_energy / (record_count * 3.0)));
    writer.set("gyro_projection_energy", finite_float(gyro_projection_energy / record_count));
    writer.set("gyro_lateral_energy", finite_float(gyro_lateral_energy / (record_count * 2.0)));
    for (size_t axis = 0; axis < 3; ++axis) {
        char name[64];
        snprintf(name, sizeof(name), "gyro_integral_%c", static_cast<char>('x' + axis));
        writer.set(name, finite_float(gyro_integral[axis]));
        snprintf(name, sizeof(name), "gyro_abs_integral_%c", static_cast<char>('x' + axis));
        writer.set(name, finite_float(gyro_abs_integral[axis]));
        snprintf(name, sizeof(name), "gyro_cumulative_absmax_%c", static_cast<char>('x' + axis));
        writer.set(name, finite_float(cumulative_absmax[axis]));
        snprintf(name, sizeof(name), "gyro_frame_integral_%zu", axis + 1);
        writer.set(name, finite_float(frame_integral[axis]));
        snprintf(name, sizeof(name), "gyro_frame_cumulative_absmax_%zu", axis + 1);
        writer.set(name, finite_float(cumulative_frame_absmax[axis]));
    }

    // Jerk, tap impulses, and peak summaries.
    double jerk_sum = 0.0, jerk_square_sum = 0.0, jerk_max = 0.0;
    size_t tap_candidates = 0, tap_count = 0;
    int last_tap = -10000;
    double previous_acc[3] = {base[0], base[1], base[2]};
    for (size_t i = 0; i < record_count; ++i) {
        const double ax = workspace->acc[0][i];
        const double ay = workspace->acc[1][i];
        const double az = workspace->acc[2][i];
        const double jerk = sqrt((ax - previous_acc[0]) * (ax - previous_acc[0]) +
                                 (ay - previous_acc[1]) * (ay - previous_acc[1]) +
                                 (az - previous_acc[2]) * (az - previous_acc[2]));
        workspace->scratch0[i] = finite_float(jerk);
        jerk_sum += jerk;
        jerk_square_sum += jerk * jerk;
        if (jerk > jerk_max) jerk_max = jerk;
        const double acc_mag = sqrt(ax * ax + ay * ay + az * az);
        const double gx = workspace->gyro[0][i];
        const double gy = workspace->gyro[1][i];
        const double gz = workspace->gyro[2][i];
        const double gyro_mag = sqrt(gx * gx + gy * gy + gz * gz);
        if (acc_mag > 1.7 && jerk > 0.60 && gyro_mag < 120.0) {
            ++tap_candidates;
            if (static_cast<int>(i) - last_tap > 35) {
                ++tap_count;
                last_tap = static_cast<int>(i);
            }
        }
        previous_acc[0] = ax;
        previous_acc[1] = ay;
        previous_acc[2] = az;
    }
    writer.set("jerk_mean", finite_float(jerk_sum / record_count));
    writer.set("jerk_std", finite_float(sqrt(jerk_square_sum / record_count -
                                               (jerk_sum / record_count) * (jerk_sum / record_count))));
    writer.set("jerk_max", finite_float(jerk_max));
    writer.set("tap_impulse_count", finite_float(tap_count));
    writer.set("tap_impulse_fraction", finite_float(static_cast<double>(tap_candidates) / record_count));
    size_t acc12 = 0, acc17 = 0, gyro40 = 0, gyro120 = 0;
    for (size_t i = 0; i < record_count; ++i) {
        const double ax = workspace->acc[0][i];
        const double ay = workspace->acc[1][i];
        const double az = workspace->acc[2][i];
        const double gx = workspace->gyro[0][i];
        const double gy = workspace->gyro[1][i];
        const double gz = workspace->gyro[2][i];
        const double acc_mag = sqrt(ax * ax + ay * ay + az * az);
        const double gyro_mag = sqrt(gx * gx + gy * gy + gz * gz);
        if (acc_mag > 1.2) ++acc12;
        if (acc_mag > 1.7) ++acc17;
        if (gyro_mag > 40.0) ++gyro40;
        if (gyro_mag > 120.0) ++gyro120;
    }
    writer.set("acc_mag_gt_1_2_count", finite_float(acc12));
    writer.set("acc_mag_gt_1_7_count", finite_float(acc17));
    writer.set("gyro_mag_gt_40_fraction", finite_float(static_cast<double>(gyro40) / record_count));
    writer.set("gyro_mag_gt_120_fraction", finite_float(static_cast<double>(gyro120) / record_count));

    // Acceleration magnitude peaks.
    for (int kind = 0; kind < 2; ++kind) {
        for (size_t i = 0; i < record_count; ++i) {
            const double x = kind == 0 ? workspace->acc[0][i] : workspace->gyro[0][i];
            const double y = kind == 0 ? workspace->acc[1][i] : workspace->gyro[1][i];
            const double z = kind == 0 ? workspace->acc[2][i] : workspace->gyro[2][i];
            workspace->scratch0[i] = finite_float(sqrt(x * x + y * y + z * z));
        }
        const PeakSummary peaks = calculate_peak_summary(workspace->scratch0, record_count, workspace, workspace->scratch1);
        const char *prefix = kind == 0 ? "acc_mag_peak" : "gyro_mag_peak";
        char name[96];
        snprintf(name, sizeof(name), "%s_count_75", prefix); writer.set(name, finite_float(peaks.count75));
        snprintf(name, sizeof(name), "%s_count_90", prefix); writer.set(name, finite_float(peaks.count90));
        snprintf(name, sizeof(name), "%s_max", prefix); writer.set(name, peaks.max);
        snprintf(name, sizeof(name), "%s_mean_prominence", prefix); writer.set(name, peaks.mean_prominence);
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        for (size_t i = 0; i < record_count; ++i) workspace->scratch0[i] = fabs(workspace->gyro[axis][i]);
        const PeakSummary peaks = calculate_peak_summary(workspace->scratch0, record_count, workspace, workspace->scratch1);
        char prefix[48];
        snprintf(prefix, sizeof(prefix), "gyro_%c_peak", static_cast<char>('x' + axis));
        char name[96];
        snprintf(name, sizeof(name), "%s_count_75", prefix); writer.set(name, finite_float(peaks.count75));
        snprintf(name, sizeof(name), "%s_count_90", prefix); writer.set(name, finite_float(peaks.count90));
        snprintf(name, sizeof(name), "%s_max", prefix); writer.set(name, peaks.max);
        snprintf(name, sizeof(name), "%s_mean_prominence", prefix); writer.set(name, peaks.mean_prominence);
    }

    // Reversal and periodicity features.
    for (size_t axis = 0; axis < 3; ++axis) {
        int last_sign = 0;
        int reversals = 0;
        for (size_t i = 0; i < record_count; ++i) {
            const float value = workspace->gyro[axis][i];
            if (fabs(value) <= 8.0f) continue;
            const int sign = value > 0.0f ? 1 : -1;
            if (last_sign && sign != last_sign) ++reversals;
            last_sign = sign;
        }
        char name[64];
        snprintf(name, sizeof(name), "gyro_%c_reversal_count", static_cast<char>('x' + axis));
        writer.set(name, finite_float(reversals));
        char prefix[48];
        snprintf(prefix, sizeof(prefix), "gyro_%c", static_cast<char>('x' + axis));
        if (!set_periodicity(writer, prefix, calculate_periodicity(workspace->gyro[axis], record_count,
                                                                  workspace->scratch0))) return false;
    }
    if (!set_periodicity(writer, "rel_acc_mag",
                         calculate_periodicity(workspace->rel_acc_mag, record_count,
                                               workspace->scratch0))) return false;
    if (!set_periodicity(writer, "gravity_angle",
                         calculate_periodicity(workspace->gravity_angle, record_count,
                                               workspace->scratch0))) return false;

    return writer.complete();
}

bool gesture_svm_predict(
    const float features[kGestureSvmFeatureCount],
    GestureSvmPrediction *out_prediction)
{
    if (!features || !out_prediction) return false;
    set_zero_prediction(out_prediction);
    float scaled[kGestureSvmFeatureCount];
    for (size_t i = 0; i < kGestureSvmFeatureCount; ++i) {
        scaled[i] = (features[i] - gesture_svm_model::kScalerMean[i]) *
                    gesture_svm_model::kScalerInvScale[i];
        if (!isfinite(scaled[i])) scaled[i] = 0.0f;
    }
    float kernel[kGestureSvmSupportVectorCount];
    for (size_t support = 0; support < kGestureSvmSupportVectorCount; ++support) {
        float squared_distance = 0.0f;
        const float *vector = &gesture_svm_model::kSupportVectors[support * kGestureSvmFeatureCount];
        for (size_t feature = 0; feature < kGestureSvmFeatureCount; ++feature) {
            const float delta = scaled[feature] - vector[feature];
            squared_distance += delta * delta;
        }
        kernel[support] = expf(-gesture_svm_model::kGamma * squared_distance);
    }
    size_t starts[kGestureSvmClassCount + 1] = {};
    for (size_t i = 0; i < kGestureSvmClassCount; ++i) {
        starts[i + 1] = starts[i] + gesture_svm_model::kSupportCounts[i];
    }
    size_t pair = 0;
    for (size_t i = 0; i < kGestureSvmClassCount; ++i) {
        for (size_t j = i + 1; j < kGestureSvmClassCount; ++j) {
            float decision = gesture_svm_model::kIntercept[pair];
            const float *coef_i = &gesture_svm_model::kDualCoefficients[(j - 1) * kGestureSvmSupportVectorCount];
            const float *coef_j = &gesture_svm_model::kDualCoefficients[i * kGestureSvmSupportVectorCount];
            for (size_t support = starts[i]; support < starts[i + 1]; ++support) {
                decision += coef_i[support] * kernel[support];
            }
            for (size_t support = starts[j]; support < starts[j + 1]; ++support) {
                decision += coef_j[support] * kernel[support];
            }
            out_prediction->pairwise_decision[pair] = decision;
            if (decision > 0.0f) ++out_prediction->votes[i];
            else ++out_prediction->votes[j];
            ++pair;
        }
    }
    size_t winner = 0;
    for (size_t i = 1; i < kGestureSvmClassCount; ++i) {
        // Strict > preserves libsvm's earliest-class tie behavior.
        if (out_prediction->votes[i] > out_prediction->votes[winner]) winner = i;
    }
    uint8_t second = 0;
    for (size_t i = 0; i < kGestureSvmClassCount; ++i) {
        if (i == winner) continue;
        if (out_prediction->votes[i] > second) second = out_prediction->votes[i];
    }
    out_prediction->model_class_index = static_cast<uint8_t>(winner);
    out_prediction->winning_vote_margin = out_prediction->votes[winner] - second;
    out_prediction->class_name = gesture_svm_model::kClassNames[winner];
    static constexpr GestureSvmEventClass kModelToEvent[kGestureSvmClassCount] = {
        GestureSvmEventClass::NOD,
        GestureSvmEventClass::NONE,
        GestureSvmEventClass::ROCK,
        GestureSvmEventClass::SPIN180,
        GestureSvmEventClass::TAP1,
        GestureSvmEventClass::TAP2,
        GestureSvmEventClass::TAP3,
        GestureSvmEventClass::TILT_LR,
        GestureSvmEventClass::TURN_LR,
    };
    out_prediction->event_class = kModelToEvent[winner];
    return true;
}

}  // namespace nk
