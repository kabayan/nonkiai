#include "motion_detector.h"

#include <math.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;

static inline float norm3(float x, float y, float z)
{
    return sqrtf(x * x + y * y + z * z);
}

static inline float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static inline float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static inline int seconds_to_samples(float seconds, float period)
{
    return (int)ceilf(seconds / ((period > 0.0001f) ? period : MotionDetector::kDt));
}

static void normalize3(float v[3])
{
    const float n = norm3(v[0], v[1], v[2]);
    if (n > 1.0e-6f) {
        v[0] /= n;
        v[1] /= n;
        v[2] /= n;
    }
}

static void quat_multiply(const float a[4], const float b[4], float out[4])
{
    out[0] = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
    out[1] = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
    out[2] = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
    out[3] = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
}

static void quat_conjugate(const float q[4], float out[4])
{
    out[0] = q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = -q[3];
}

static void quat_rotate(const float q[4], const float v[3], float out[3])
{
    const float tx = 2.0f * (q[2] * v[2] - q[3] * v[1]);
    const float ty = 2.0f * (q[3] * v[0] - q[1] * v[2]);
    const float tz = 2.0f * (q[1] * v[1] - q[2] * v[0]);
    out[0] = v[0] + q[0] * tx + (q[2] * tz - q[3] * ty);
    out[1] = v[1] + q[0] * ty + (q[3] * tx - q[1] * tz);
    out[2] = v[2] + q[0] * tz + (q[1] * ty - q[2] * tx);
}

static void relative_rotate(const float reference_q[4], const float current_q[4],
                            const float v[3], float out[3])
{
    float reference_conj[4];
    float relative[4];
    quat_conjugate(reference_q, reference_conj);
    quat_multiply(reference_conj, current_q, relative);
    quat_rotate(relative, v, out);
}
}

const char *nk_motion_event_name(nk_motion_event_t event)
{
    switch (event) {
        case NK_MOTION_TAP1:           return "tap1";
        case NK_MOTION_TAP2:           return "tap2";
        case NK_MOTION_TAP3:           return "tap3";
        case NK_MOTION_NOD:            return "nod";
        case NK_MOTION_ROCK:           return "rock";
        case NK_MOTION_SPIN180:        return "spin180";
        case NK_MOTION_TILT_LR:        return "tilt_lr";
        case NK_MOTION_TURN_LR:        return "turn_lr";
        case NK_MOTION_CALIB_START:    return "calib_start";
        case NK_MOTION_CALIB_COMPLETE: return "calib_ok";
        case NK_MOTION_CALIB_FAILED:   return "calib_ng";
        case NK_MOTION_WAKEUP:         return "wakeup";
        case NK_MOTION_NONE:
        default:                       return "none";
    }
}

MotionDetector::MotionDetector()
{
    reset();
}

void MotionDetector::reset()
{
    state_ = State::IDLE;
    settle_countdown_ = 0;
    quiz_mode_ = false;
    managed_calibration_ = false;
    is_rocked_ = false;
    lying_sample_count_ = 0;

    upright_base_[0] = upright_base_[1] = upright_base_[2] = 0.0f;
    gesture_base_[0] = 0.0f;
    gesture_base_[1] = 0.0f;
    gesture_base_[2] = 1.0f;
    reference_ready_ = false;
    angle_from_upright_ = 0.0f;
    startup_calib_count_ = 0;
    startup_quiet_count_ = 0;
    startup_sum_[0] = startup_sum_[1] = startup_sum_[2] = 0.0;

    fusion_.reset();
    fusion_.begin(1.0f / kDt);
    base_q_[0] = gesture_q_[0] = 1.0f;
    base_q_[1] = base_q_[2] = base_q_[3] = 0.0f;
    gesture_q_[1] = gesture_q_[2] = gesture_q_[3] = 0.0f;
    gesture_gravity_base_[0] = last_gravity_base_[0] = last_gravity_gesture_[0] = 0.0f;
    gesture_gravity_base_[1] = last_gravity_base_[1] = last_gravity_gesture_[1] = 0.0f;
    gesture_gravity_base_[2] = last_gravity_base_[2] = last_gravity_gesture_[2] = 1.0f;
    fused_gravity_sensor_[0] = fused_gravity_sensor_[1] = 0.0f;
    fused_gravity_sensor_[2] = 1.0f;
    sample_period_s_ = kDt;
    fusion_resync_pending_ = false;
    resync_quiet_count_ = 0;
    resync_sum_[0] = resync_sum_[1] = resync_sum_[2] = 0.0;

    start_acc_count_ = start_acc_head_ = 0;
    start_acc_last_sample_ = -1000;
    sample_idx_ = 0;
    last_tap_sample_ = -1000;
    tap_window_remaining_ = 0;
    tap_count_ = 0;
    tap_train_suppressed_ = button_suppressed_ = false;
    prev_a_vec_[0] = 0.0f;
    prev_a_vec_[1] = 0.0f;
    prev_a_vec_[2] = 1.0f;

    calib_sample_countdown_ = 0;
    calib_quiet_count_ = 0;
    calib_sum_ax_ = calib_sum_ay_ = calib_sum_az_ = 0.0;
    calib_anchor_[0] = 0.0f;
    calib_anchor_[1] = 0.0f;
    calib_anchor_[2] = 1.0f;
    calib_anchor_valid_ = false;

    gesture_start_idx_ = 0;
    yaw_integral_ = 0.0f;
    max_yaw_ = 0.0f;
    max_angle_delta_ = 0.0f;
    quiet_samples_ = 0;
    gesture_sample_count_ = 0;
    roll_integral_ = max_roll_angle_ = 0.0f;
    max_roll_rate_ = max_pitch_rate_ = 0.0f;
    max_gx_ = max_gy_ = max_gz_ = 0.0f;
    sum_gy_ = sum_gy_sq_ = sum_cross_sq_ = 0.0;
    sum_horizontal_sq_ = sum_vertical_sq_ = 0.0;
    pitch_sign_ = pitch_reversals_ = 0;
    pitch_lobe_angle_ = 0.0f;
    horizontal_streak_samples_ = 0;
    horizontal_pitch_sign_ = 0;
    horizontal_pitch_reversals_ = 0;
    horizontal_pitch_lobe_angle_ = 0.0f;
    horizontal_axis_base_[0] = 1.0f;
    horizontal_axis_base_[1] = horizontal_axis_base_[2] = 0.0f;
    horizontal_axis_ready_ = false;

    end_acc_head_ = 0;
    end_acc_count_ = 0;
    pending_event_ = NK_MOTION_NONE;
}

void MotionDetector::set_sample_period(float sample_period_s)
{
    if (sample_period_s < 0.0001f || sample_period_s > 0.1f) return;
    sample_period_s_ = sample_period_s;
    fusion_.set_sample_period(sample_period_s_);
}

void MotionDetector::begin_calibration()
{
    discard_input();
    state_ = State::CALIBRATING;
    calib_sample_countdown_ = seconds_to_samples(3.0f, sample_period_s_);
    calib_quiet_count_ = 0;
    calib_sum_ax_ = calib_sum_ay_ = calib_sum_az_ = 0.0;
    calib_anchor_valid_ = false;
}

void MotionDetector::force_wakeup()
{
    is_rocked_ = false;
    lying_sample_count_ = 0;
    discard_input();
}

void MotionDetector::discard_input()
{
    start_acc_count_ = start_acc_head_ = 0;
    start_acc_last_sample_ = -1000;
    if (state_ != State::CALIBRATING) {
        state_ = State::SETTLE;
        settle_countdown_ = seconds_to_samples(0.35f, sample_period_s_);
    }
    tap_train_suppressed_ = false;
    tap_count_ = tap_window_remaining_ = 0;
    last_tap_sample_ = sample_idx_;
    gesture_sample_count_ = 0;
    end_acc_count_ = 0;
    pending_event_ = NK_MOTION_NONE;
    fusion_resync_pending_ = true;
    resync_quiet_count_ = 0;
    resync_sum_[0] = resync_sum_[1] = resync_sum_[2] = 0.0;
}

void MotionDetector::set_base_acc(float ax, float ay, float az)
{
    const float n = norm3(ax, ay, az);
    if (n < 0.5f) return;
    start_acc_count_ = start_acc_head_ = 0;
    start_acc_last_sample_ = -1000;
    upright_base_[0] = ax;
    upright_base_[1] = ay;
    upright_base_[2] = az;
    gesture_base_[0] = ax;
    gesture_base_[1] = ay;
    gesture_base_[2] = az;
    reference_ready_ = true;
    angle_from_upright_ = 0.0f;
    fusion_.reset_from_gravity(ax, ay, az);
    fusion_.quaternion(base_q_);
    fusion_.quaternion(gesture_q_);
    fusion_.gravity_sensor(fused_gravity_sensor_);
    normalize3(fused_gravity_sensor_);
    gesture_gravity_base_[0] = last_gravity_base_[0] = last_gravity_gesture_[0] = fused_gravity_sensor_[0];
    gesture_gravity_base_[1] = last_gravity_base_[1] = last_gravity_gesture_[1] = fused_gravity_sensor_[1];
    gesture_gravity_base_[2] = last_gravity_base_[2] = last_gravity_gesture_[2] = fused_gravity_sensor_[2];
    fusion_resync_pending_ = false;
    resync_quiet_count_ = 0;
    resync_sum_[0] = resync_sum_[1] = resync_sum_[2] = 0.0;
}

void MotionDetector::rotate_current_to_base(float x, float y, float z,
                                             float *out_x, float *out_y, float *out_z) const
{
    float current_q[4];
    fusion_.quaternion(current_q);
    const float v[3] = {x, y, z};
    float out[3];
    relative_rotate(base_q_, current_q, v, out);
    *out_x = out[0];
    *out_y = out[1];
    *out_z = out[2];
}

nk_motion_event_t MotionDetector::update(int16_t ax, int16_t ay, int16_t az,
                                         int16_t gx, int16_t gy, int16_t gz)
{
    return update(ax, ay, az, gx, gy, gz, kDt);
}

nk_motion_event_t MotionDetector::update(int16_t ax, int16_t ay, int16_t az,
                                         int16_t gx, int16_t gy, int16_t gz,
                                         float sample_period_s)
{
    set_sample_period(sample_period_s);
    ++sample_idx_;

    const float a_x = ax * kAccelRes;
    const float a_y = ay * kAccelRes;
    const float a_z = az * kAccelRes;
    const float g_x = gx * kGyroRes;
    const float g_y = gy * kGyroRes;
    const float g_z = gz * kGyroRes;
    const float a_mag = norm3(a_x, a_y, a_z);
    const float g_mag = norm3(g_x, g_y, g_z);
    const float jerk = norm3(a_x - prev_a_vec_[0],
                             a_y - prev_a_vec_[1],
                             a_z - prev_a_vec_[2]);
    prev_a_vec_[0] = a_x;
    prev_a_vec_[1] = a_y;
    prev_a_vec_[2] = a_z;

    // Learn the plush toy's actual startup pose once, while it is held still.
    // No tap or gesture is accepted before this reference exists.
    if (!reference_ready_) {
        // A static startup pose is required before any event is accepted.
        // Keep the gyro gate tight enough that a slow turn is not mistaken
        // for the one-second gravity reference window.
        const bool quiet = g_mag < 8.0f && fabsf(a_mag - 1.0f) < 0.15f && jerk < 0.04f;
        if (quiet) {
            ++startup_calib_count_;
            ++startup_quiet_count_;
            startup_sum_[0] += a_x;
            startup_sum_[1] += a_y;
            startup_sum_[2] += a_z;
        } else {
            startup_calib_count_ = 0;
            startup_quiet_count_ = 0;
            startup_sum_[0] = startup_sum_[1] = startup_sum_[2] = 0.0;
        }
        const int startup_target = seconds_to_samples(1.0f, sample_period_s_);
        if (startup_quiet_count_ >= startup_target) {
            const float n = (float)startup_quiet_count_;
            set_base_acc((float)(startup_sum_[0] / n),
                         (float)(startup_sum_[1] / n),
                         (float)(startup_sum_[2] / n));
            startup_calib_count_ = startup_quiet_count_ = 0;
            startup_sum_[0] = startup_sum_[1] = startup_sum_[2] = 0.0;
#ifdef ESP_PLATFORM
            ESP_LOGI("nk-motion", "Startup upright calibrated (gravity=%.3f,%.3f,%.3f)",
                     upright_base_[0], upright_base_[1], upright_base_[2]);
#endif
        }
        return NK_MOTION_NONE;
    }

    // Dynamic acceleration is not a gravity observation. The gyro still
    // propagates attitude, while Madgwick feedback is gated until |a| settles.
    const bool accel_trusted = fabsf(a_mag - 1.0f) < 0.20f;
    const bool resync_was_pending = fusion_resync_pending_;
    if (fusion_resync_pending_) {
        // Speech/cue pauses can contain movement. Wait for a genuinely quiet
        // window before seeding; a tap/shock must never become the new gravity.
        if (!accel_trusted || g_mag >= 8.0f || jerk >= 0.05f) {
            // The required window is continuous. A movement sample must not
            // leave an earlier pose in the average used for reseeding.
            resync_quiet_count_ = 0;
            resync_sum_[0] = resync_sum_[1] = resync_sum_[2] = 0.0;
        } else {
            ++resync_quiet_count_;
            resync_sum_[0] += a_x;
            resync_sum_[1] += a_y;
            resync_sum_[2] += a_z;
            const int required = seconds_to_samples(0.02f, sample_period_s_);
            if (resync_quiet_count_ >= required) {
                const float n = (float)resync_quiet_count_;
                fusion_.reset_from_gravity((float)(resync_sum_[0] / n),
                                           (float)(resync_sum_[1] / n),
                                           (float)(resync_sum_[2] / n));
                fusion_resync_pending_ = false;
                resync_quiet_count_ = 0;
                resync_sum_[0] = resync_sum_[1] = resync_sum_[2] = 0.0;
            }
        }
        // For ordinary gesture input, retain the raw onset-relative detector
        // path during a long moving post-pause interval; only the
        // mount-independent rock gate remains disabled until reseed clears.
        // Calibration below still advances its fixed deadline and counts only
        // raw quiet samples, so movement cannot postpone failure indefinitely.
    }

    fusion_.update_imu(g_x, g_y, g_z,
                       accel_trusted && !fusion_resync_pending_ ? a_x : 0.0f,
                       accel_trusted && !fusion_resync_pending_ ? a_y : 0.0f,
                       accel_trusted && !fusion_resync_pending_ ? a_z : 0.0f);
    fusion_.gravity_sensor(fused_gravity_sensor_);
    normalize3(fused_gravity_sensor_);

    // Gravity is already expressed in the current sensor axes.  Comparing it
    // with the saved startup/gesture gravity vector is the relative posture
    // measurement; rotating both through q_current would cancel the motion
    // and incorrectly make every posture appear upright.
    float gravity_base[3] = {
        fused_gravity_sensor_[0], fused_gravity_sensor_[1], fused_gravity_sensor_[2],
    };
    float upright_unit[3] = {upright_base_[0], upright_base_[1], upright_base_[2]};
    normalize3(upright_unit);
    const float upright_dot = clampf(dot3(gravity_base, upright_unit), -1.0f, 1.0f);
    const float angle_from_upright = acosf(upright_dot) * kRadToDeg;
    angle_from_upright_ = angle_from_upright;
    last_gravity_base_[0] = gravity_base[0];
    last_gravity_base_[1] = gravity_base[1];
    last_gravity_base_[2] = gravity_base[2];

    // A continuous five-second lying posture enters sleep without requiring
    // a rocking gesture. Quiz mode remains isolated from product sleep state.
    if (!is_rocked_ && !quiz_mode_) {
        if (angle_from_upright >= 60.0f) {
            ++lying_sample_count_;
            if (lying_sample_count_ >= seconds_to_samples(5.0f, sample_period_s_)) {
                lying_sample_count_ = 0;
                is_rocked_ = true;
                state_ = State::IDLE;
                tap_train_suppressed_ = false;
                tap_count_ = tap_window_remaining_ = 0;
                pending_event_ = NK_MOTION_NONE;
                return NK_MOTION_ROCK;
            }
        } else {
            lying_sample_count_ = 0;
        }
    }

    float gesture_gravity[3] = {
        fused_gravity_sensor_[0], fused_gravity_sensor_[1], fused_gravity_sensor_[2],
    };
    last_gravity_gesture_[0] = gesture_gravity[0];
    last_gravity_gesture_[1] = gesture_gravity[1];
    last_gravity_gesture_[2] = gesture_gravity[2];
    // The fused gravity is authoritative for the horizontal rock gate. Keep
    // the established raw onset-relative angle for nod/tilt/look features;
    // after a pause the fusion seed may legitimately lag the first gesture.
    const float gesture_norm = norm3(gesture_base_[0], gesture_base_[1], gesture_base_[2]);
    const float raw_gesture_dot = (a_x * gesture_base_[0] +
                                   a_y * gesture_base_[1] +
                                   a_z * gesture_base_[2]) /
                                  (a_mag * gesture_norm + 1.0e-6f);
    const float angle_delta = acosf(clampf(raw_gesture_dot, -1.0f, 1.0f)) * kRadToDeg;

    float wx_base, wy_base, wz_base;
    rotate_current_to_base(g_x, g_y, g_z, &wx_base, &wy_base, &wz_base);
    const float omega_vert = wx_base * upright_unit[0] +
                             wy_base * upright_unit[1] +
                             wz_base * upright_unit[2];
    const float hx = wx_base - omega_vert * upright_unit[0];
    const float hy = wy_base - omega_vert * upright_unit[1];
    const float hz = wz_base - omega_vert * upright_unit[2];
    const float horizontal_rate = norm3(hx, hy, hz);

    // Preserve the established tilt/nod features for the calibrated product
    // mount. Rock alone uses the full horizontal vector above, so its
    // back-and-forth test does not depend on sensor Y.
    const float gux = gesture_base_[0] / (gesture_norm + 1.0e-6f);
    const float guy = gesture_base_[1] / (gesture_norm + 1.0e-6f);
    const float guz = gesture_base_[2] / (gesture_norm + 1.0e-6f);
    const float gesture_omega_vert = g_x * gux + g_y * guy + g_z * guz;
    const float lateral_norm = sqrtf(gux * gux + guz * guz);
    const float roll_rate = lateral_norm > 0.1f
        ? (-guz * g_x + gux * g_z) / lateral_norm : 0.0f;
    const float pitch_rate = lateral_norm > 0.1f
        ? (g_y - guy * gesture_omega_vert) / lateral_norm : 0.0f;

    // Keep a short local holding pose for gesture onset only. It never alters
    // upright_base_, so sleep/wakeup remain tied to the one-shot calibration.
    if ((state_ == State::IDLE || state_ == State::SETTLE) &&
        g_mag < 15.0f && fabsf(a_mag - 1.0f) < 0.1f && jerk < 0.04f) {
        if (start_acc_count_ &&
            (sample_idx_ - start_acc_last_sample_ > seconds_to_samples(0.2f, sample_period_s_) ||
             norm3(a_x - start_acc_buf_[(start_acc_head_ + kStartWindow - 1) % kStartWindow][0],
                   a_y - start_acc_buf_[(start_acc_head_ + kStartWindow - 1) % kStartWindow][1],
                   a_z - start_acc_buf_[(start_acc_head_ + kStartWindow - 1) % kStartWindow][2]) > 0.05f)) {
            start_acc_count_ = start_acc_head_ = 0;
        }
        start_acc_buf_[start_acc_head_][0] = a_x;
        start_acc_buf_[start_acc_head_][1] = a_y;
        start_acc_buf_[start_acc_head_][2] = a_z;
        start_acc_head_ = (start_acc_head_ + 1) % kStartWindow;
        if (start_acc_count_ < kStartWindow) ++start_acc_count_;
        start_acc_last_sample_ = sample_idx_;
    }

    // -------------------------------------------------------------
    // Calibration state (3回タップ後 3秒静止で基準点設定)
    // -------------------------------------------------------------
    if (state_ == State::CALIBRATING) {
        --calib_sample_countdown_;
        if (g_mag < 20.0f && fabsf(a_mag - 1.0f) < 0.15f && a_mag > 0.5f) {
            const float anchor_norm = norm3(calib_anchor_[0], calib_anchor_[1], calib_anchor_[2]);
            const float direction_dot = calib_anchor_valid_
                ? (a_x * calib_anchor_[0] + a_y * calib_anchor_[1] + a_z * calib_anchor_[2]) /
                      (a_mag * anchor_norm + 1.0e-6f)
                : 1.0f;
            // Do not average two held poses into a fictitious middle pose.
            // A 15-degree cone tolerates sensor noise while requiring the
            // three-second calibration window to describe one posture.
            if (!calib_anchor_valid_ || direction_dot < 0.966f ||
                norm3(a_x - calib_anchor_[0], a_y - calib_anchor_[1], a_z - calib_anchor_[2]) > 0.20f) {
                calib_anchor_[0] = a_x;
                calib_anchor_[1] = a_y;
                calib_anchor_[2] = a_z;
                calib_anchor_valid_ = true;
                calib_quiet_count_ = 0;
                calib_sum_ax_ = calib_sum_ay_ = calib_sum_az_ = 0.0;
            }
            ++calib_quiet_count_;
            calib_sum_ax_ += a_x;
            calib_sum_ay_ += a_y;
            calib_sum_az_ += a_z;
        }
        if (calib_sample_countdown_ <= 0) {
            state_ = State::IDLE;
            const int required = (int)ceilf(0.6f * seconds_to_samples(3.0f, sample_period_s_));
            if (calib_quiet_count_ >= required) {
                const float n = (float)calib_quiet_count_;
                const float new_ax = (float)(calib_sum_ax_ / n);
                const float new_ay = (float)(calib_sum_ay_ / n);
                const float new_az = (float)(calib_sum_az_ / n);
                if (norm3(new_ax, new_ay, new_az) < 0.5f) {
                    return NK_MOTION_CALIB_FAILED;
                }
                set_base_acc(new_ax, new_ay, new_az);
                is_rocked_ = false;
#ifdef ESP_PLATFORM
                ESP_LOGI("nk-motion", "Calibrated upright=%.3f,%.3f,%.3f", new_ax, new_ay, new_az);
#endif
                return NK_MOTION_CALIB_COMPLETE;
            }
            return NK_MOTION_CALIB_FAILED;
        }
        return NK_MOTION_NONE;
    }

    // Returning to the same upright/lying boundary used by the WebUI wakes
    // automatically. Require a quiet gravity sample so a passing swing does
    // not wake the toy.
    const float raw_wake_dot = (a_x * upright_base_[0] + a_y * upright_base_[1] + a_z * upright_base_[2]) /
                               (a_mag * norm3(upright_base_[0], upright_base_[1], upright_base_[2]) + 1.0e-6f);
    const float raw_wake_angle = acosf(clampf(raw_wake_dot, -1.0f, 1.0f)) * kRadToDeg;
    if (is_rocked_ && raw_wake_angle <= 30.0f && g_mag < 25.0f &&
        fabsf(a_mag - 1.0f) < 0.15f) {
        is_rocked_ = false;
        lying_sample_count_ = 0;
        start_acc_count_ = start_acc_head_ = 0;
        start_acc_last_sample_ = -1000;
        tap_count_ = 0;
        tap_window_remaining_ = 0;
        state_ = State::SETTLE;
        settle_countdown_ = seconds_to_samples(0.35f, sample_period_s_);
        fusion_resync_pending_ = false;
        return NK_MOTION_WAKEUP;
    }

    // -------------------------------------------------------------
    // Settle state
    // -------------------------------------------------------------
    if (state_ == State::SETTLE) {
        --settle_countdown_;
        if (settle_countdown_ <= 0 && g_mag < 30.0f && fabsf(a_mag - 1.0f) < 0.15f) {
            state_ = State::IDLE;
        }
        return NK_MOTION_NONE;
    }

    // -------------------------------------------------------------
    // Tap handling (Decision A-1: Tap Priority)
    // -------------------------------------------------------------
    const bool is_tap_impulse = (a_mag > 1.7f) && (jerk > 0.60f) &&
                                (g_mag < 120.0f) &&
                                ((sample_idx_ - last_tap_sample_) > seconds_to_samples(0.0875f, sample_period_s_));
    if (is_tap_impulse) {
        start_acc_count_ = start_acc_head_ = 0;
        start_acc_last_sample_ = -1000;
        last_tap_sample_ = sample_idx_;
        if (state_ != State::TAP_COUNTING) tap_train_suppressed_ = false;
        ++tap_count_;
        state_ = State::TAP_COUNTING;
        tap_window_remaining_ = seconds_to_samples(0.42f, sample_period_s_);
        gesture_sample_count_ = 0;
        end_acc_count_ = 0;
    }

    if (state_ == State::TAP_COUNTING) {
        if (tap_filter_ && tap_filter_(tap_filter_context_, static_cast<uint32_t>(
                (sample_idx_-last_tap_sample_)*sample_period_s_*1000.0f + 0.5f)))
            tap_train_suppressed_ = true;
        --tap_window_remaining_;
        if (tap_window_remaining_ <= 0) {
            if (tap_train_suppressed_) {
                discard_input();
                tap_train_suppressed_ = false;
                button_suppressed_ = true;
                return NK_MOTION_NONE;
            }
            nk_motion_event_t evt = NK_MOTION_NONE;
            if (tap_count_ == 1) {
                evt = NK_MOTION_TAP1;
                if (!is_rocked_) {
                    // DLS-108: the tap impulse itself is already 420 ms old;
                    // use this settled sample as the new 0-degree pose. The
                    // normal tap1 event/speech remains, with no calibration cue.
                    set_base_acc(a_x, a_y, a_z);
                }
                state_ = State::SETTLE;
                settle_countdown_ = seconds_to_samples(0.50f, sample_period_s_);
            } else if (tap_count_ == 2) {
                evt = NK_MOTION_TAP2;
                state_ = State::SETTLE;
                settle_countdown_ = seconds_to_samples(0.50f, sample_period_s_);
            } else if (tap_count_ >= 3) {
                evt = NK_MOTION_TAP3;
                if (is_rocked_) {
                    state_ = State::SETTLE;
                    settle_countdown_ = seconds_to_samples(0.50f, sample_period_s_);
                } else if (!quiz_mode_ && !managed_calibration_) begin_calibration();
                else discard_input();
            }
            tap_count_ = 0;
            return evt;
        }
    }

    // -------------------------------------------------------------
    // Gesture handling
    // -------------------------------------------------------------
    if (state_ == State::IDLE) {
        if (g_mag > 40.0f) {
            state_ = State::GESTURE_ACTIVE;
            gesture_start_idx_ = sample_idx_;
            yaw_integral_ = 0.0f;
            max_yaw_ = 0.0f;
            max_angle_delta_ = 0.0f;
            quiet_samples_ = 0;
            gesture_sample_count_ = 0;
            roll_integral_ = max_roll_angle_ = 0.0f;
            max_roll_rate_ = fabsf(roll_rate);
            max_pitch_rate_ = fabsf(pitch_rate);
            max_gx_ = fabsf(g_x);
            max_gy_ = fabsf(g_y);
            max_gz_ = fabsf(g_z);
            pitch_sign_ = pitch_reversals_ = 0;
            pitch_lobe_angle_ = 0.0f;
            horizontal_streak_samples_ = 0;
            horizontal_pitch_sign_ = 0;
            horizontal_pitch_reversals_ = 0;
            horizontal_pitch_lobe_angle_ = 0.0f;
            horizontal_axis_base_[0] = 1.0f;
            horizontal_axis_base_[1] = horizontal_axis_base_[2] = 0.0f;
            horizontal_axis_ready_ = false;
            sum_horizontal_sq_ = sum_vertical_sq_ = 0.0;
            track_pitch(g_y);
            sum_gy_ = g_y;
            sum_gy_sq_ = (double)g_y * g_y;
            sum_cross_sq_ = (double)g_x * g_x + (double)g_z * g_z;
            end_acc_head_ = 0;
            end_acc_count_ = 1;
            end_acc_buf_[0][0] = a_x;
            end_acc_buf_[0][1] = a_y;
            end_acc_buf_[0][2] = a_z;
            ++gesture_sample_count_;

            fusion_.quaternion(gesture_q_);
            gesture_gravity_base_[0] = fused_gravity_sensor_[0];
            gesture_gravity_base_[1] = fused_gravity_sensor_[1];
            gesture_gravity_base_[2] = fused_gravity_sensor_[2];
            last_gravity_gesture_[0] = gesture_gravity_base_[0];
            last_gravity_gesture_[1] = gesture_gravity_base_[1];
            last_gravity_gesture_[2] = gesture_gravity_base_[2];
            gesture_base_[0] = a_x;
            gesture_base_[1] = a_y;
            gesture_base_[2] = a_z;
            if (start_acc_count_ && sample_idx_ - start_acc_last_sample_ <= seconds_to_samples(0.2f, sample_period_s_)) {
                for (int j = 0; j < 3; ++j) {
                    float sum = 0.0f;
                    for (size_t i = 0; i < start_acc_count_; ++i) sum += start_acc_buf_[i][j];
                    gesture_base_[j] = sum / (float)start_acc_count_;
                }
            }
#ifdef ESP_PLATFORM
            ESP_LOGI("nk-motion", "Gesture origin=%.3f,%.3f,%.3f upright=%.3f,%.3f,%.3f",
                     gesture_base_[0], gesture_base_[1], gesture_base_[2],
                     upright_base_[0], upright_base_[1], upright_base_[2]);
#endif
            start_acc_count_ = start_acc_head_ = 0;
        }
    } else if (state_ == State::GESTURE_ACTIVE) {
        ++gesture_sample_count_;
        // Yaw/turn classification retains the raw gyro projection relative
        // to the gesture's onset pose. The fusion-derived vector is reserved
        // for the mount-independent rock test below.
        yaw_integral_ += gesture_omega_vert * sample_period_s_;
        if (fabsf(yaw_integral_) > max_yaw_) max_yaw_ = fabsf(yaw_integral_);
        if (angle_delta > max_angle_delta_) max_angle_delta_ = angle_delta;

        // Rock uses the vector h; existing tilt features remain for the
        // product mount and the legacy labelled gesture regression.
        roll_integral_ += roll_rate * sample_period_s_;
        if (fabsf(roll_integral_) > max_roll_angle_) max_roll_angle_ = fabsf(roll_integral_);
        if (fabsf(roll_rate) > max_roll_rate_) max_roll_rate_ = fabsf(roll_rate);
        if (fabsf(pitch_rate) > max_pitch_rate_) max_pitch_rate_ = fabsf(pitch_rate);
        if (fabsf(g_x) > max_gx_) max_gx_ = fabsf(g_x);
        if (fabsf(g_y) > max_gy_) max_gy_ = fabsf(g_y);
        if (fabsf(g_z) > max_gz_) max_gz_ = fabsf(g_z);
        track_pitch(g_y);
        // A horizontal lobe observed before a pause reseed is not evidence
        // for a later rock. Drop both its direction history and energy, and
        // restart only after a fresh fusion seed has been accepted.
        if (fusion_resync_pending_ || resync_was_pending) {
            horizontal_streak_samples_ = 0;
            horizontal_pitch_sign_ = 0;
            horizontal_pitch_reversals_ = 0;
            horizontal_pitch_lobe_angle_ = 0.0f;
            horizontal_axis_base_[0] = 1.0f;
            horizontal_axis_base_[1] = horizontal_axis_base_[2] = 0.0f;
            horizontal_axis_ready_ = false;
            sum_horizontal_sq_ = 0.0;
            sum_vertical_sq_ = 0.0;
        } else {
            track_horizontal_pitch(angle_from_upright, hx, hy, hz);
            sum_horizontal_sq_ += (double)horizontal_rate * horizontal_rate;
            sum_vertical_sq_ += (double)omega_vert * omega_vert;
        }
        sum_gy_ += g_y;
        sum_gy_sq_ += (double)g_y * g_y;
        sum_cross_sq_ += (double)g_x * g_x + (double)g_z * g_z;

        end_acc_buf_[end_acc_head_][0] = a_x;
        end_acc_buf_[end_acc_head_][1] = a_y;
        end_acc_buf_[end_acc_head_][2] = a_z;
        end_acc_head_ = (end_acc_head_ + 1) % kEndAccWindow;
        if (end_acc_count_ < kEndAccWindow) ++end_acc_count_;
        if (g_mag < 30.0f) ++quiet_samples_;
        else quiet_samples_ = 0;

        const int dur_samples = sample_idx_ - gesture_start_idx_;
        if ((quiet_samples_ > seconds_to_samples(0.35f, sample_period_s_) &&
             dur_samples > seconds_to_samples(0.30f, sample_period_s_)) ||
            dur_samples > seconds_to_samples(2.5f, sample_period_s_)) {
            classify_gesture();
            state_ = State::SETTLE;
            settle_countdown_ = seconds_to_samples(0.35f, sample_period_s_);
            nk_motion_event_t evt = pending_event_;
            pending_event_ = NK_MOTION_NONE;
            return evt;
        }
    }

    return NK_MOTION_NONE;
}

void MotionDetector::track_pitch(float gy)
{
    if (fabsf(gy) <= 25.0f) return;
    const int sign = gy > 0.0f ? 1 : -1;
    if (pitch_sign_ != 0 && sign != pitch_sign_) {
        if (pitch_lobe_angle_ >= 3.0f) ++pitch_reversals_;
        else pitch_reversals_ = 0;
        pitch_lobe_angle_ = 0.0f;
    }
    pitch_sign_ = sign;
    pitch_lobe_angle_ += fabsf(gy) * sample_period_s_;
}

void MotionDetector::track_horizontal_pitch(float angle_from_upright,
                                             float hx, float hy, float hz)
{
    const bool horizontal = angle_from_upright >= 60.0f && angle_from_upright <= 120.0f;
    if (!horizontal) {
        horizontal_streak_samples_ = 0;
        horizontal_pitch_sign_ = 0;
        horizontal_pitch_reversals_ = 0;
        horizontal_pitch_lobe_angle_ = 0.0f;
        horizontal_axis_ready_ = false;
        return;
    }
    ++horizontal_streak_samples_;
    const float h_norm = norm3(hx, hy, hz);
    if (!horizontal_axis_ready_ && h_norm > 25.0f) {
        horizontal_axis_base_[0] = hx / h_norm;
        horizontal_axis_base_[1] = hy / h_norm;
        horizontal_axis_base_[2] = hz / h_norm;
        horizontal_axis_ready_ = true;
    }
    if (!horizontal_axis_ready_) return;
    const float scalar = hx * horizontal_axis_base_[0] +
                         hy * horizontal_axis_base_[1] +
                         hz * horizontal_axis_base_[2];
    if (fabsf(scalar) <= 25.0f) return;
    const int sign = scalar > 0.0f ? 1 : -1;
    if (horizontal_pitch_sign_ != 0 && sign != horizontal_pitch_sign_) {
        if (horizontal_pitch_lobe_angle_ >= 3.0f) ++horizontal_pitch_reversals_;
        else horizontal_pitch_reversals_ = 0;
        horizontal_pitch_lobe_angle_ = 0.0f;
    }
    horizontal_pitch_sign_ = sign;
    horizontal_pitch_lobe_angle_ += fabsf(scalar) * sample_period_s_;
}

void MotionDetector::classify_gesture()
{
    pending_event_ = NK_MOTION_NONE;
    if (gesture_sample_count_ < 60) return;

    const double mean_gy = sum_gy_ / gesture_sample_count_;
    const double var_gy = (sum_gy_sq_ / gesture_sample_count_) - (mean_gy * mean_gy);
    [[maybe_unused]] const float gy_std = (var_gy > 0.0) ? sqrtf((float)var_gy) : 0.0f;

    // Return-to-origin for non-rock gestures remains a raw accelerometer
    // measurement against the immutable onset holding pose. The fused vector
    // can be transiently stale immediately after a speech pause.
    float end_acc_x = 0.0f;
    float end_acc_y = 0.0f;
    float end_acc_z = 0.0f;
    for (size_t i = 0; i < end_acc_count_; ++i) {
        end_acc_x += end_acc_buf_[i][0];
        end_acc_y += end_acc_buf_[i][1];
        end_acc_z += end_acc_buf_[i][2];
    }
    const float inv_end_count = 1.0f / static_cast<float>(end_acc_count_);
    end_acc_x *= inv_end_count;
    end_acc_y *= inv_end_count;
    end_acc_z *= inv_end_count;
    const float base_norm = norm3(gesture_base_[0], gesture_base_[1], gesture_base_[2]);
    const float end_norm = norm3(end_acc_x, end_acc_y, end_acc_z);
    const float ret_dot = (end_acc_x * gesture_base_[0] +
                           end_acc_y * gesture_base_[1] +
                           end_acc_z * gesture_base_[2]) /
                          (end_norm * base_norm + 1.0e-6f);
    const float ret_angle = acosf(clampf(ret_dot, -1.0f, 1.0f)) * kRadToDeg;
    const float fused_end_norm = norm3(last_gravity_base_[0], last_gravity_base_[1],
                                       last_gravity_base_[2]);
    const float up_norm = norm3(upright_base_[0], upright_base_[1], upright_base_[2]);
    const float up_dot = (last_gravity_base_[0] * upright_base_[0] +
                          last_gravity_base_[1] * upright_base_[1] +
                          last_gravity_base_[2] * upright_base_[2]) /
                         (fused_end_norm * up_norm + 1.0e-6f);
    const float end_upright_angle = acosf(clampf(up_dot, -1.0f, 1.0f)) * kRadToDeg;
#ifdef ESP_PLATFORM
    ESP_LOGI("nk-motion", "Classify yaw=%.1f tilt=%.1f roll=%.1f return=%.1f upright=%.1f roll_rate=%.1f pitch_rate=%.1f pitch_std=%.1f reversals=%d horiz_samples=%d horiz_reversals=%d horiz_energy=%.3f vert_energy=%.3f",
             max_yaw_, max_angle_delta_, max_roll_angle_, ret_angle, end_upright_angle,
             max_roll_rate_, max_pitch_rate_, gy_std, pitch_reversals_,
             horizontal_streak_samples_, horizontal_pitch_reversals_,
             (float)sqrt(sum_horizontal_sq_), (float)sqrt(sum_vertical_sq_));
#endif

    const bool substantial_horizontal = 3.0 * sum_horizontal_sq_ >= sum_vertical_sq_;
    const bool ended_horizontal = end_upright_angle >= 60.0f && end_upright_angle <= 120.0f;
    const bool sustained_horizontal = horizontal_streak_samples_ >= seconds_to_samples(0.5f, sample_period_s_);
    const bool horizontal_repeated = horizontal_pitch_reversals_ >= 2;
    if (!fusion_resync_pending_ &&
        ended_horizontal && sustained_horizontal &&
        horizontal_repeated && substantial_horizontal &&
        max_yaw_ < 120.0f && gesture_sample_count_ > seconds_to_samples(0.5f, sample_period_s_)) {
        pending_event_ = NK_MOTION_ROCK;
        return;
    }
    if (max_yaw_ >= 120.0f) {
        pending_event_ = NK_MOTION_SPIN180;
        return;
    }
    if (ret_angle > 18.0f) return;

    if (max_angle_delta_ > 25.0f && max_roll_angle_ > 18.0f &&
        max_yaw_ < max_angle_delta_ * 0.5f &&
        max_roll_rate_ > max_pitch_rate_ * 0.8f) {
        pending_event_ = swap_nod_tilt_ ? NK_MOTION_NOD : NK_MOTION_TILT_LR;
        return;
    }
    if (max_yaw_ > 22.0f) {
        pending_event_ = NK_MOTION_TURN_LR;
        return;
    }
    if (max_angle_delta_ > 30.0f && max_gy_ > max_gx_ * 1.5f) {
        pending_event_ = swap_nod_tilt_ ? NK_MOTION_TILT_LR : NK_MOTION_NOD;
        return;
    }
}

nk_motion_event_t MotionDetector::flush()
{
    if (state_ == State::TAP_COUNTING) {
        if (tap_train_suppressed_) {
            discard_input();
            tap_train_suppressed_ = false;
            button_suppressed_ = true;
            return NK_MOTION_NONE;
        }
        nk_motion_event_t evt = NK_MOTION_NONE;
        if (tap_count_ == 1) evt = NK_MOTION_TAP1;
        else if (tap_count_ == 2) evt = NK_MOTION_TAP2;
        else if (tap_count_ >= 3) {
            evt = NK_MOTION_TAP3;
            if (!quiz_mode_ && !managed_calibration_) begin_calibration();
            else discard_input();
            tap_count_ = 0;
            return evt;
        }
        tap_count_ = 0;
        state_ = State::SETTLE;
        settle_countdown_ = seconds_to_samples(0.50f, sample_period_s_);
        return evt;
    }
    if (state_ == State::GESTURE_ACTIVE) {
        classify_gesture();
        state_ = State::SETTLE;
        settle_countdown_ = seconds_to_samples(0.35f, sample_period_s_);
        nk_motion_event_t evt = pending_event_;
        pending_event_ = NK_MOTION_NONE;
        return evt;
    }
    return NK_MOTION_NONE;
}
