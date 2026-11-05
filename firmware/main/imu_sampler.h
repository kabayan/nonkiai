#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// BMI270 / BMM150 を掴み、ODR を設定する。nk_board_init() の後に呼ぶこと
// (M5.begin 自体は board 側が持つ)。
bool nk_imu_init(void);

// サンプリングタスクを起動する (core 1 固定。core 0 は BLE が使う)。
bool nk_imu_start_task(void);

void nk_imu_set_streaming(bool on);
bool nk_imu_is_streaming(void);

// BMI270 の ACC_CONF / GYR_CONF を直接書いて ODR を変える。
// M5Unified は ODR を設定せず POR 既定 (accel 100Hz / gyro 200Hz) のままなので、
// 400Hz が必要ならここで上書きする必要がある。
// 受け付けるのは 100 / 200 / 400 / 800 / 1600 Hz。
bool nk_imu_set_odr(uint16_t hz);
uint16_t nk_imu_get_odr(void);

uint32_t nk_imu_seq(void);
uint32_t nk_imu_dropped(void);

// 専用設定画面へ返す検出器の状態スナップショット。
uint8_t nk_imu_motion_state(void);
bool nk_imu_is_sleeping(void);
uint32_t nk_imu_last_tap_ms(void);
void nk_imu_note_button(uint32_t now_ms, bool pressed);
// 姿勢表示: 0=未確定, 1=起立(<=30°), 2=寝姿勢(>=60°), 3=中間。
// 0=基準待ち, 1=起立, 2=寝姿勢。角度は正立基準からの centidegree。
uint8_t nk_imu_posture_state(void);
uint16_t nk_imu_posture_angle_cdeg(void);
void nk_imu_set_swap_nod_tilt(bool enabled);
bool nk_imu_swap_nod_tilt(void);

// INFO characteristic 用。ADC→物理量のスケール係数を返す。
void nk_imu_convert_param(float *accel_res, float *gyro_res, float *mag_res);

// 直近検出されたジェスチャ動作名（無ければ "none"）
const char *nk_imu_last_motion_name(void);

// DLS-024: 音声合成中のセンサーサスペンド（音声全集中・Core 1 開放）
void nk_imu_suspend(void);
void nk_imu_resume(void);
bool nk_imu_is_suspended(void);
// stt32 infer callback: atomic flag only, independent of audio ownership.
void nk_imu_set_inference_busy(bool busy);
// After suspend, wait for an in-flight IMU read before remapping shared I2C.
bool nk_imu_wait_idle(uint32_t timeout_ms);

// Development CTRL asks the sampler to run the ordinary awake tap1 path.
// Only posts a request; rejects sleep, quiz and an already busy interaction.
bool nk_imu_request_listen(bool dump_pcm);
// 起動語の待受（stt32 DLS-1082）。wanted: 起きていてクイズ中でない。interaction_busy: 動作の発話・聞き取りの途中。
bool nk_imu_standby_wanted(void);
bool nk_imu_interaction_busy(void);

#ifdef __cplusplus
}
#endif
