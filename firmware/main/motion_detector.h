#pragma once

#include <stdint.h>
#include <stddef.h>

#include "madgwick_ahrs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NK_MOTION_NONE = 0,
    NK_MOTION_TAP1,
    NK_MOTION_TAP2,
    NK_MOTION_TAP3,
    NK_MOTION_NOD,
    NK_MOTION_ROCK,
    NK_MOTION_SPIN180,
    NK_MOTION_TILT_LR,
    NK_MOTION_TURN_LR,
    NK_MOTION_CALIB_START,    // 3回タップ後のキャリブレーション待機開始
    NK_MOTION_CALIB_COMPLETE, // 3秒静止による基準点学習完了
    NK_MOTION_CALIB_FAILED,   // 静止せず失敗
    NK_MOTION_WAKEUP,         // 寝かしつけ (rock) 後に正立へ復帰
} nk_motion_event_t;

const char *nk_motion_event_name(nk_motion_event_t event);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

class MotionDetector {
public:
    static constexpr float kAccelRes = 0.000244140625f; // +-8g -> 1/4096 g/LSB
    static constexpr float kGyroRes  = 0.0610351562f;    // +-2000dps -> 1/16.384 dps/LSB
    static constexpr float kDt       = 1.0f / 400.0f;    // 400Hz (2.5ms)

    enum class State {
        IDLE,
        TAP_COUNTING,
        CALIBRATING,
        GESTURE_ACTIVE,
        SETTLE,
    };

    MotionDetector();

    // Call with raw ADC values. Returns event when classified, or NK_MOTION_NONE.
    // The optional period is the configured IMU period; it is explicit so a
    // non-400Hz stream cannot silently integrate with a 400Hz quaternion step.
    nk_motion_event_t update(int16_t ax, int16_t ay, int16_t az,
                             int16_t gx, int16_t gy, int16_t gz);
    nk_motion_event_t update(int16_t ax, int16_t ay, int16_t az,
                             int16_t gx, int16_t gy, int16_t gz,
                             float sample_period_s);

    // Called while counting with the latest impulse's age. A matching impulse
    // latches suppression for the entire train, before any calibration side effect.
    using TapFilter = bool (*)(void *, uint32_t age_ms);
    void set_tap_filter(TapFilter filter, void *context) { tap_filter_ = filter; tap_filter_context_ = context; }
    bool take_button_suppressed() { bool v = button_suppressed_; button_suppressed_ = false; return v; }
    void reset();
    void set_interaction_mode(bool quiz, bool managed_calibration = true) {
        quiz_mode_ = quiz;
        managed_calibration_ = managed_calibration;
    }
    void begin_calibration();
    // Discard pre-speech partial input without changing posture or sleep.
    void discard_input();
    void set_base_acc(float ax, float ay, float az);
    void set_sample_period(float sample_period_s);
    bool pose_ready() const { return reference_ready_; }
    float angle_from_upright() const { return angle_from_upright_; }
    State state() const { return state_; }
    bool sleeping() const { return is_rocked_; }
    void force_wakeup();
    void set_swap_nod_tilt(bool enabled) { swap_nod_tilt_ = enabled; }
    nk_motion_event_t flush();

private:
    void classify_gesture();
    void track_pitch(float gy);
    void track_horizontal_pitch(float angle_from_upright,
                                float hx, float hy, float hz);
    void rotate_current_to_base(float x, float y, float z,
                                float *out_x, float *out_y, float *out_z) const;

    static constexpr size_t kEndAccWindow = 60;

    State state_ = State::IDLE;
    int settle_countdown_ = 0;
    bool quiz_mode_ = false;
    bool managed_calibration_ = false;
    bool is_rocked_ = false;  // 寝かしつけ (rock) 発火済みフラグ
    int lying_sample_count_ = 0;
    // Product configuration sets this before sampling starts. Keep the core
    // detector's standalone default backward-compatible for host fixtures.
    bool swap_nod_tilt_ = false;

    // Upright reference gravity vector in the sensor frame at calibration.
    // It is intentionally unknown until startup quiet calibration or set_base_acc.
    float upright_base_[3] = {0.0f, 0.0f, 0.0f};
    bool reference_ready_ = false;
    float angle_from_upright_ = 0.0f;
    int startup_calib_count_ = 0;
    int startup_quiet_count_ = 0;
    double startup_sum_[3] = {};

    // Madgwick q maps the current sensor frame to the auxiliary frame. The
    // calibration quaternion maps the startup sensor frame to that same frame.
    // Their relative rotation gives a mount-independent base-frame gyro.
    MadgwickAhrs fusion_;
    float base_q_[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float gesture_q_[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float gesture_gravity_base_[3] = {0.0f, 0.0f, 1.0f};
    float last_gravity_base_[3] = {0.0f, 0.0f, 1.0f};
    float last_gravity_gesture_[3] = {0.0f, 0.0f, 1.0f};
    float fused_gravity_sensor_[3] = {0.0f, 0.0f, 1.0f};
    float sample_period_s_ = kDt;
    bool fusion_resync_pending_ = false;
    int resync_quiet_count_ = 0;
    double resync_sum_[3] = {};
    // Per-gesture origin: captured once at onset, immutable until next gesture.
    float gesture_base_[3] = {0.0f, 0.0f, 1.0f};
    // Short pre-onset gravity history, never a calibration/reference update.
    static constexpr size_t kStartWindow = 40; // 100 ms at 400 Hz
    float start_acc_buf_[kStartWindow][3] = {};
    size_t start_acc_head_ = 0;
    size_t start_acc_count_ = 0;
    int32_t start_acc_last_sample_ = -1000;

    // Calibration: 3-second quiet window after tap3
    int calib_sample_countdown_ = 0;
    int calib_quiet_count_ = 0;
    double calib_sum_ax_ = 0.0;
    double calib_sum_ay_ = 0.0;
    double calib_sum_az_ = 0.0;
    float calib_anchor_[3] = {0.0f, 0.0f, 1.0f};
    bool calib_anchor_valid_ = false;

    TapFilter tap_filter_ = nullptr;
    void *tap_filter_context_ = nullptr;
    bool tap_train_suppressed_ = false, button_suppressed_ = false;
    // Tap tracking
    int32_t sample_idx_ = 0;
    int32_t last_tap_sample_ = -1000;
    int tap_window_remaining_ = 0;
    int tap_count_ = 0;
    float prev_a_vec_[3] = {0.0f, 0.0f, -1.0f};

    // Gesture tracking
    int32_t gesture_start_idx_ = 0;
    float yaw_integral_ = 0.0f;
    float max_yaw_ = 0.0f;
    float max_angle_delta_ = 0.0f;
    int quiet_samples_ = 0;
    int gesture_sample_count_ = 0;

    float roll_integral_ = 0.0f;
    float max_roll_angle_ = 0.0f;
    float max_roll_rate_ = 0.0f;
    float max_pitch_rate_ = 0.0f;
    float max_gx_ = 0.0f;
    float max_gy_ = 0.0f;
    float max_gz_ = 0.0f;
    double sum_gy_ = 0.0;
    double sum_gy_sq_ = 0.0;
    double sum_cross_sq_ = 0.0;
    int pitch_sign_ = 0;
    int pitch_reversals_ = 0;
    float pitch_lobe_angle_ = 0.0f;

    // Rocking is valid only while the calibrated upright reference is held
    // near horizontal. Keep a separate interval-local counter so reversals
    // collected while upright cannot qualify a later brief horizontal pass.
    int horizontal_streak_samples_ = 0;
    int horizontal_pitch_sign_ = 0;
    int horizontal_pitch_reversals_ = 0;
    float horizontal_pitch_lobe_angle_ = 0.0f;
    float horizontal_axis_base_[3] = {1.0f, 0.0f, 0.0f};
    bool horizontal_axis_ready_ = false;
    double sum_horizontal_sq_ = 0.0;
    double sum_vertical_sq_ = 0.0;

    // Circular buffer for end 60 acc samples
    float end_acc_buf_[kEndAccWindow][3] = {};
    size_t end_acc_head_ = 0;
    size_t end_acc_count_ = 0;

    nk_motion_event_t pending_event_ = NK_MOTION_NONE;
};

#endif // __cplusplus
