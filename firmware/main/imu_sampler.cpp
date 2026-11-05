#include "imu_sampler.h"

#include <string.h>
#include <atomic>

#include <M5Unified.hpp>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_stream.h"
#include "wire_format.h"
#include "motion_detector.h"
#include "button_tap_window.h"
#include "gesture_shadow.h"
#include "motion_interaction.h"
#include "audio_cue.h"
#include "voice_config.h"
#include "voice_player.h"
#include "voice_listen.h"
#include "direction_ir.h"
#include "direction_ir_policy.h"
#include "listen_messages.h"
#include "task_stack_sizes.h"

static const char *const kTag = "nk-imu";

// BMI270 レジスタ (M5Unified は ODR を設定しないので直接叩く)
static constexpr uint8_t kBmi270ChipIdReg = 0x00;
static constexpr uint8_t kBmi270ChipId    = 0x24;
static constexpr uint8_t kAccConfReg      = 0x40;
static constexpr uint8_t kGyrConfReg      = 0x42;
static constexpr uint32_t kI2cFreq        = 400000;

static uint8_t  s_bmi270_addr = 0;
static uint16_t s_odr_hz      = 400;
static volatile bool s_streaming = false;
static std::atomic<bool> s_suspended{false};
static std::atomic<bool> s_inference_busy{false}, s_reading{false};
static volatile uint32_t s_seq = 0;
static volatile uint32_t s_dropped = 0;
static volatile uint8_t s_motion_state = 0;
static volatile bool s_motion_sleeping = false;
static volatile uint32_t s_last_tap_ms = 0;
static volatile uint8_t s_posture_state = 0;
static volatile uint16_t s_posture_angle_cdeg = 0;
static volatile bool s_swap_nod_tilt = true;
static bool s_imu_ready = false;

static ButtonTapWindow s_button_taps;
void nk_imu_note_button(uint32_t now_ms, bool pressed) { s_button_taps.note(now_ms, pressed); }
static bool suppress_button_tap(void *, uint32_t age_ms) {
    // Capture wall time at the impulse, not reconstructed sample time: task
    // scheduling delays must not move an older tap across a button boundary.
    static uint32_t tap_ms = 0;
    if (age_ms == 0) tap_ms = static_cast<uint32_t>(esp_timer_get_time()/1000);
    return s_button_taps.contains(tap_ms);
}
static MotionDetector s_detector;
static MotionInteraction s_interaction;
static std::atomic<bool> s_speech_pending{false};
static bool s_listen_pending = false; // sampler-owned, guards tracked polling
static std::atomic<bool> s_quiz_active{false};
static std::atomic<uint8_t> s_listen_request{0}; // 0=none, 1=capture, 2=capture+dump
static nk_motion_event_t s_speech_event = NK_MOTION_NONE;
static uint32_t s_speech_seq = 0;
static int64_t s_cue_until_us = 0;

static void perform_action(MotionInteraction::Action action)
{
    using Cue = MotionInteraction::Cue;
    switch (action.cue) {
    case Cue::Tap3: nk_audio_beep(880, 80, 1175, 80); s_cue_until_us = esp_timer_get_time() + 160000; break;
    case Cue::Calibrated: nk_audio_beep(660, 100, 1320, 150); s_cue_until_us = esp_timer_get_time() + 250000; break;
    case Cue::Failed: nk_audio_beep(330, 200, 0, 0); s_cue_until_us = esp_timer_get_time() + 200000; break;
    case Cue::Wakeup:
    case Cue::Exit: nk_audio_beep(523, 100, 659, 150); s_cue_until_us = esp_timer_get_time() + 250000; break;
    default: break;
    }
    s_detector.set_interaction_mode(s_interaction.quiz());
    s_quiz_active.store(s_interaction.quiz());
    if (action.calibrate) s_detector.begin_calibration();
    if (action.text != nullptr) {
        bool accepted = false;
        if (*action.text) accepted = voice_player_say_tracked(action.text, action.exact);
        nk_ble_notify_status_motion((uint8_t)s_speech_event, s_speech_seq, (uint32_t)esp_timer_get_time());
        nk_ble_notify_motion_speech((uint8_t)s_speech_event, s_speech_seq, action.text, accepted);
        s_speech_pending = accepted;
        if (*action.text && !accepted) {
            // A rejected prompt must never enable answers/calibration.
            perform_action(s_interaction.speech_finished(esp_timer_get_time(), false));
        }
    }
    s_motion_state = static_cast<uint8_t>(s_detector.state());
    s_motion_sleeping = s_detector.sleeping();
}

static void begin_listen(uint8_t event, uint32_t seq)
{
    // Set the sampler pause before a core0 worker can touch the codec bus.
    s_speech_pending.store(true);
    s_listen_pending = true;
    nk_ble_notify_status_motion(event, seq, static_cast<uint32_t>(esp_timer_get_time()));
    if (!nk_voice_listen_start(event, seq)) {
        s_listen_pending = false;
        const bool accepted = voice_player_say_tracked(kListenRetry, true);
        nk_ble_notify_intent(event, seq, 255, 3, 255, 255, 0, 0);
        nk_ble_notify_motion_speech(event, seq, kListenRetry, accepted);
        s_speech_pending.store(accepted);
    }
}

static const char *s_last_motion_name = "none";

static uint8_t shadow_pause_reason()
{
    if (nk_imu_is_suspended()) return NK_SHADOW_RESET_SUSPENDED;
    if (s_speech_pending) return NK_SHADOW_RESET_SPEECH_PENDING;
    if (voice_player_is_playing()) return NK_SHADOW_RESET_SPEECH_PLAYING;
    return NK_SHADOW_RESET_CUE;
}

const char *nk_imu_last_motion_name(void)
{
    return s_last_motion_name;
}

void nk_imu_suspend(void)
{
    s_suspended = true;
    ESP_LOGI(kTag, "IMU suspended (audio priority)");
    nk_ble_notify_status_detector_state();
}

void nk_imu_resume(void)
{
    s_suspended = false;
    ESP_LOGI(kTag, "IMU resumed");
    nk_ble_notify_status_detector_state();
}

bool nk_imu_is_suspended(void)
{
    return s_suspended.load() || s_inference_busy.load();
}

void nk_imu_set_inference_busy(bool busy)
{
    s_inference_busy.store(busy);
}

bool nk_imu_wait_idle(uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    while (s_reading.load()) {
        if (esp_timer_get_time() >= deadline) return false;
        vTaskDelay(1);
    }
    return true;
}

bool nk_imu_request_listen(bool dump_pcm)
{
    if (!s_imu_ready || s_motion_sleeping || s_quiz_active.load() || s_speech_pending.load()
        || nk_imu_is_suspended() || voice_player_is_playing() || nk_voice_listen_busy()) return false;
    uint8_t expected = 0;
    return s_listen_request.compare_exchange_strong(expected, dump_pcm ? 2 : 1);
}

// stt32 DLS-1082: 起動語の待受が使う。待受を続けてよいか（起きていてクイズ中でない）。
bool nk_imu_standby_wanted(void)
{
    return s_imu_ready && !s_motion_sleeping && !s_quiz_active.load();
}

// 動作の発話・聞き取りの途中か。待受は終わるまで入らない。
bool nk_imu_interaction_busy(void)
{
    return s_speech_pending.load() || s_listen_pending || s_listen_request.load() != 0;
}

// ODR の値 → BMI270 の odr フィールド (ACC_CONF / GYR_CONF 共通の bits[3:0])
static bool odr_to_bits(uint16_t hz, uint8_t *bits)
{
    switch (hz) {
        case 100:  *bits = 0x08; return true;
        case 200:  *bits = 0x09; return true;
        case 400:  *bits = 0x0A; return true;
        case 800:  *bits = 0x0B; return true;
        case 1600: *bits = 0x0C; return true;
        default:   return false;
    }
}

static bool find_bmi270(void)
{
    for (uint8_t addr : {(uint8_t)0x69, (uint8_t)0x68}) {
        if (m5::In_I2C.readRegister8(addr, kBmi270ChipIdReg, kI2cFreq) == kBmi270ChipId) {
            s_bmi270_addr = addr;
            ESP_LOGI(kTag, "BMI270 found at 0x%02X", addr);
            return true;
        }
    }
    ESP_LOGE(kTag, "BMI270 not found on internal I2C");
    return false;
}

bool nk_imu_set_odr(uint16_t hz)
{
    uint8_t bits;
    if (!odr_to_bits(hz, &bits) || s_bmi270_addr == 0) {
        return false;
    }

    // ACC_CONF: bit7 filter_perf=1 / bits[6:4] bwp=0b010 (norm_avg4) / bits[3:0] odr
    const uint8_t acc_conf = (uint8_t)(0xA0 | bits);
    // GYR_CONF: bit7 filter_perf=1 / bit6 noise_perf=0 / bits[5:4] bwp=0b10 (normal) / bits[3:0] odr
    const uint8_t gyr_conf = (uint8_t)(0xA0 | bits);

    bool ok = m5::In_I2C.writeRegister8(s_bmi270_addr, kAccConfReg, acc_conf, kI2cFreq)
           && m5::In_I2C.writeRegister8(s_bmi270_addr, kGyrConfReg, gyr_conf, kI2cFreq);
    if (!ok) {
        ESP_LOGE(kTag, "failed to write ACC_CONF/GYR_CONF");
        return false;
    }

    const uint8_t acc_rb = m5::In_I2C.readRegister8(s_bmi270_addr, kAccConfReg, kI2cFreq);
    const uint8_t gyr_rb = m5::In_I2C.readRegister8(s_bmi270_addr, kGyrConfReg, kI2cFreq);
    ESP_LOGI(kTag, "ODR %uHz  ACC_CONF=0x%02X (rb 0x%02X)  GYR_CONF=0x%02X (rb 0x%02X)",
             (unsigned)hz, acc_conf, acc_rb, gyr_conf, gyr_rb);

    s_odr_hz = hz;
    // Keep the attitude integrator's period tied to the configured BMI270
    // stream. The detector still resynchronizes its quaternion after pauses.
    s_detector.set_sample_period(1.0f / (float)hz);
    nk_gesture_shadow_rate_changed(hz);
    return true;
}

uint16_t nk_imu_get_odr(void) { return s_odr_hz; }
uint32_t nk_imu_seq(void) { return s_seq; }
uint32_t nk_imu_dropped(void) { return s_dropped; }
uint8_t nk_imu_motion_state(void) { return s_motion_state; }
bool nk_imu_is_sleeping(void) { return s_motion_sleeping; }
uint32_t nk_imu_last_tap_ms(void) { return s_last_tap_ms; }
uint8_t nk_imu_posture_state(void) { return s_posture_state; }
uint16_t nk_imu_posture_angle_cdeg(void) { return s_posture_angle_cdeg; }
void nk_imu_set_swap_nod_tilt(bool enabled)
{
    s_swap_nod_tilt = enabled;
    s_detector.set_swap_nod_tilt(enabled);
}
bool nk_imu_swap_nod_tilt(void) { return s_swap_nod_tilt; }

void nk_imu_set_streaming(bool on) { s_streaming = on; }
bool nk_imu_is_streaming(void) { return s_streaming; }

void nk_imu_convert_param(float *accel_res, float *gyro_res, float *mag_res)
{
    // BMI270_Class::getConvertParam は mag_res / temp のみ上書きし、accel / gyro は
    // imu_convert_param_t の既定値 (±8g / ±2000dps) に委ねている。
    m5::IMU_Base::imu_convert_param_t param;
    auto *inst = M5.Imu.getImuInstancePtr(0);
    if (inst) {
        inst->getConvertParam(&param);
    }
    if (accel_res) *accel_res = param.accel_res;
    if (gyro_res)  *gyro_res  = param.gyro_res;
    if (mag_res)   *mag_res   = param.mag_res;
}

bool nk_imu_init(void)
{
    // M5.begin() は nk_board_init() が済ませている前提。スピーカー (Voice Base) の
    // 設定も同じ M5.config に載るため、初期化を board 側へ一本化した。
    if (!M5.Imu.isEnabled()) {
        ESP_LOGE(kTag, "M5.Imu not enabled (board=%d)", (int)M5.getBoard());
        return false;
    }
    ESP_LOGI(kTag, "IMU type=%d board=%d", (int)M5.Imu.getType(), (int)M5.getBoard());

    // 生値を送るのが本基盤の存在理由なので、M5Unified の自動オフセット調整を明示的に切る。
    // (begin() 内で一度 255 を入れてから 0 に戻しているが、意図を残すため明示する)
    M5.Imu.setCalibration(0, 0, 0);

    if (!find_bmi270()) {
        return false;
    }
    if (!nk_imu_set_odr(s_odr_hz)) {
        ESP_LOGW(kTag, "ODR setup failed; running at BMI270 POR default");
    }

    s_imu_ready = true;
    return true;
}

static void sampler_task(void *arg)
{
    (void)arg;

    uint8_t buf[NK_PKT_HEADER_BYTES + NK_SAMPLE_BYTES * NK_MAX_SAMPLES_PER_PKT];
    auto *hdr = reinterpret_cast<nk_pkt_header_t *>(buf);
    auto *samples = reinterpret_cast<nk_sample_t *>(buf + NK_PKT_HEADER_BYTES);

    uint8_t n = 0;
    int64_t pkt_open_us = 0;
    int64_t diagnostic_us = 0;
    int64_t posture_notify_us = 0;

    s_detector.set_interaction_mode(false);
    s_detector.set_tap_filter(suppress_button_tap, nullptr);
    bool input_paused = false;
    for (;;) {
        const int64_t interaction_now = esp_timer_get_time();
        if (s_speech_pending && (!s_listen_pending || nk_voice_listen_reply_ready())
            && voice_player_tracked_status() != NK_SPEECH_PENDING) {
            // An empty configured reply is complete without an audio queue item.
            const bool completed = (s_listen_pending && nk_voice_listen_reply_silent())
                || ((!s_listen_pending || nk_voice_listen_reply_queued())
                    && voice_player_tracked_status() == NK_SPEECH_DONE);
            // DLS-143 / stt32 DLS-1082: send the recognized direction once, only after the reply finished
            // normally and only if the mode still sends IR (toy; it did at recognition too).
            uint8_t ir_direction = NK_IR_AUTO_NONE;
            if (s_listen_pending) {
                ir_direction = nk_ir_auto_decide(nk_voice_listen_ir_direction(), completed,
                                                 nk_listen_mode_sends_ir(nk_voice_config_get_listen_mode()));
                nk_voice_listen_finish();
                s_listen_pending = false;
            }
            if (ir_direction != NK_IR_AUTO_NONE) {
                const esp_err_t ir_err = nk_ir_request_auto(ir_direction);
                if (ir_err != ESP_OK) {
                    ESP_LOGW(kTag, "auto IR direction=%u not sent: %s (no retry)",
                             ir_direction, esp_err_to_name(ir_err));
                }
            }
            s_speech_pending = false;
            s_detector.discard_input();
            perform_action(s_interaction.speech_finished(interaction_now, completed));
        }
        perform_action(s_interaction.tick(interaction_now));
        const bool listen_sampling = s_listen_pending && nk_voice_listen_sampling();
        if (nk_imu_is_suspended() || (s_speech_pending && !listen_sampling) || voice_player_is_playing()
            || interaction_now < s_cue_until_us) {
            // A request racing another interaction must not run later as a
            // surprise recording after that interaction finishes.
            s_listen_request.store(0);
            if (!input_paused) {
                nk_gesture_shadow_reset(shadow_pause_reason());
            }
            input_paused = true;
            n = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (input_paused) {
            s_detector.discard_input();
            input_paused = false;
        }

        const uint8_t request = s_listen_request.exchange(0);
        if (request != 0 && !s_detector.sleeping() && !s_interaction.quiz()) {
            nk_voice_listen_set_pcm_dump(request == 2);
            s_speech_event = NK_MOTION_TAP1;
            s_speech_seq = s_seq;
            ESP_LOGI(kTag, "diagnostic listen request (seq=%lu dump=%u)",
                     static_cast<unsigned long>(s_seq), request == 2);
            begin_listen(static_cast<uint8_t>(s_speech_event), s_speech_seq);
            continue;
        }

        // DLS-037: 本体のジェスチャ発声は BLE 収録状態に依存させない。
        // 配信停止中のサンプルはパケットへ持ち越さない。
        const bool transmit = s_streaming && nk_ble_stream_ready();
        if (!transmit) n = 0;

        // Publish before rechecking the gate: after suspend + wait_idle the
        // audio worker can safely remap I2C, even if this iteration raced it.
        s_reading.store(true);
        if (nk_imu_is_suspended() || (s_listen_pending && !nk_voice_listen_sampling())) {
            s_reading.store(false);
            continue;
        }
        const auto mask = M5.Imu.update();

        // accel の新着を 1 サンプルの基準にする。gyro / mag は直近のラッチ値を同梱。
        if (mask & m5::IMU_Class::sensor_mask_accel) {
            if (n == 0) {
                hdr->seq_start = s_seq;
                hdr->t_us = (uint32_t)esp_timer_get_time();
                hdr->odr_hz = s_odr_hz;
                pkt_open_us = esp_timer_get_time();
            }
            auto &s = samples[n];
            s.ax = M5.Imu.getRawData(0);
            s.ay = M5.Imu.getRawData(1);
            s.az = M5.Imu.getRawData(2);
            s.gx = M5.Imu.getRawData(3);
            s.gy = M5.Imu.getRawData(4);
            s.gz = M5.Imu.getRawData(5);
            s.mx = M5.Imu.getRawData(6);
            s.my = M5.Imu.getRawData(7);
            s.mz = M5.Imu.getRawData(8);
            s_reading.store(false);
            ++n;
            s_seq = s_seq + 1;

            // During microphone capture retain raw BLE samples, but suppress
            // gesture/shadow actions until the entire listen/reply finishes.
            if (!s_listen_pending) {
                nk::GestureSvmSample shadow_sample = {
                    s.ax, s.ay, s.az, s.gx, s.gy, s.gz,
                };
                nk_gesture_shadow_push(&shadow_sample, s_seq,
                                       (uint32_t)esp_timer_get_time(), s_odr_hz);

                // 400Hz オンデバイス動作判定更新 (P4 / DLS-017)
                const bool was_sleeping = s_detector.sleeping();
                nk_motion_event_t evt = s_detector.update(s.ax, s.ay, s.az, s.gx, s.gy, s.gz,
                                                          1.0f / (float)s_odr_hz);
                if (s_detector.take_button_suppressed())
                    ESP_LOGI(kTag, "[TAP] ボタン操作の前後なので捨てた");
                s_motion_state = static_cast<uint8_t>(s_detector.state());
                s_motion_sleeping = s_detector.sleeping();
                const int64_t now_us = esp_timer_get_time();
                if (s_detector.pose_ready()) {
                    const float angle = s_detector.angle_from_upright();
                    const float bounded = angle < 0.0f ? 0.0f : (angle > 180.0f ? 180.0f : angle);
                    s_posture_angle_cdeg = static_cast<uint16_t>(bounded * 100.0f + 0.5f);
                    // Observation-only threshold. Deliberately independent of
                    // gesture events and the latched sleep state (DLS-105).
                    // 30..60 degrees is a deliberate hysteresis band: it is
                    // neither upright nor lying, and does not flip sleep state.
                    s_posture_state = angle <= 30.0f ? 1 : (angle >= 60.0f ? 2 : 3);
                } else {
                    s_posture_state = 0;
                    s_posture_angle_cdeg = 0;
                }
                if (now_us - posture_notify_us >= 100000) {
                    posture_notify_us = now_us;
                    nk_ble_notify_status_posture();
                }
                if (now_us - diagnostic_us >= 2000000) {
                    diagnostic_us = now_us;
                    ESP_LOGI(kTag, "[MOTION STATE] seq=%lu state=%d sleep=%d acc=%d,%d,%d gyro=%d,%d,%d",
                             (unsigned long)s_seq, (int)s_detector.state(), (int)s_detector.sleeping(),
                             s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
                    nk_ble_notify_status_detector_state();
                }
                if (evt != NK_MOTION_NONE) {
                    if (evt == NK_MOTION_TAP1 || evt == NK_MOTION_TAP2 || evt == NK_MOTION_TAP3) {
                        s_last_tap_ms = (uint32_t)(now_us / 1000);
                    }
                    if (was_sleeping && evt == NK_MOTION_TAP3) {
                        s_detector.force_wakeup();
                        s_motion_state = static_cast<uint8_t>(s_detector.state());
                        s_motion_sleeping = false;
                        ESP_LOGI(kTag, "sleep tap3 -> forced wake and upright recalibration prompt");
                    }
                    s_last_motion_name = nk_motion_event_name(evt);
                    ESP_LOGI(kTag, ">>> MOTION DETECTED: %s (seq=%lu) <<<", s_last_motion_name, (unsigned long)s_seq);

                    s_speech_event = evt;
                    s_speech_seq = s_seq;
                    // Queue the comparison before this event may start speech and
                    // suspend the sampler. The event item follows its sample in
                    // the bounded ring.
                    nk_gesture_shadow_event(evt, s_seq);
                    // Sequential phrases retain the original motion/sequence association.
                    if (was_sleeping && evt != NK_MOTION_WAKEUP && evt != NK_MOTION_TAP3) {
                        perform_action({nk_voice_config_get_sleep_talk()});
                    } else if (evt == NK_MOTION_TAP1 && !s_interaction.quiz()) {
                        begin_listen(static_cast<uint8_t>(evt), s_seq);
                    } else {
                        const char *normal_text = s_interaction.quiz() || evt == NK_MOTION_SPIN180
                            ? nullptr : nk_voice_config_get_text(evt);
                        perform_action(s_interaction.event(evt, now_us, normal_text, esp_random));
                    }
                    nk_ble_notify_status_detector_state();
                }
            }
        } else {
            s_reading.store(false);
        }

        const uint8_t n_max = nk_ble_max_samples_per_pkt();
        const bool full = (n >= n_max);
        // 低 ODR でもブラウザを待たせないための保険。400Hz なら full が先に来る。
        const bool stale = (n > 0) && (esp_timer_get_time() - pkt_open_us > 100000);

        if (!transmit) {
            n = 0;
        } else if (full || stale) {
            hdr->n = n;
            const size_t len = NK_PKT_HEADER_BYTES + (size_t)n * NK_SAMPLE_BYTES;
            if (!nk_ble_notify_stream(buf, len)) {
                // 送れなかったパケットは捨てる。ブラウザ側は seq のギャップで
                // 欠損を検出するので、ここで詰まらせるより落とす方が筋が通る。
                s_dropped = s_dropped + 1;
            }
            n = 0;
        }

        vTaskDelay(1);  // CONFIG_FREERTOS_HZ=1000 前提の 1ms ポーリング
    }
}

bool nk_imu_start_task(void)
{
    if (!s_imu_ready) {
        return false;
    }
    // BLE (NimBLE host) は core 0。サンプリングは core 1 に固定して奪い合いを避ける。
    // Shadow feature extraction runs as a lower-priority worker and never
    // blocks this acquisition path.
    if (!nk_gesture_shadow_start_task()) {
        ESP_LOGW(kTag, "shadow worker unavailable; current detector remains active");
    }
    return xTaskCreatePinnedToCore(sampler_task, "nk_sampler", nk::kSamplerStackBytes, nullptr, 6, nullptr, 1) == pdPASS;
}
