#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "device_weather.h"

#ifdef __cplusplus
extern "C" {
#endif

// NimBLE peripheral を起動する。IMU / M5Unified の初期化より先に呼ぶこと
// (notif DLS-190: BLE controller は内部 DRAM の連続ブロックを要求する)。
esp_err_t nk_ble_init(void);

// 接続済み かつ STREAM characteristic の notify が購読されている
bool nk_ble_stream_ready(void);

// notify 送出。送れなかったら false (呼び手は捨てて dropped を数える)
bool nk_ble_notify_stream(const uint8_t *data, size_t len);

// ネゴ済み MTU から 1 パケットに載るサンプル数を出す
uint8_t nk_ble_max_samples_per_pkt(void);

void nk_ble_notify_status_marker(uint32_t marker_id, uint32_t seq, uint32_t t_us);
void nk_ble_notify_status_state(void);
void nk_ble_notify_status_beep(uint32_t seq, uint32_t t_us, uint16_t freq_hz, uint16_t ms);
void nk_ble_notify_status_motion(uint8_t event_id, uint32_t seq, uint32_t t_us);
// Selected text for exactly this motion sequence; split to fit default ATT MTU.
void nk_ble_notify_motion_speech(uint8_t event_id, uint32_t seq, const char *text, bool accepted);
void nk_ble_notify_intent(uint8_t event_id, uint32_t seq, uint8_t intent, uint8_t reason,
                          uint8_t raw1, uint8_t raw2, float score1, float score2);
void nk_ble_notify_status_detector_state(void);
void nk_ble_notify_status_posture(void);
void nk_ble_notify_swap_nod_tilt(void);
void nk_ble_notify_listen_mode(void);

// 全アクションの音声設定を STATUS characteristic 経由で 1 件ずつ順次 notify
void nk_ble_notify_all_voice_configs(void);

// DLS-021: 音量設定を STATUS characteristic で notify
void nk_ble_notify_voice_volume(uint8_t volume);
void nk_ble_notify_ir_result(uint8_t direction, uint32_t request, int32_t err, uint32_t generation);

// 専用設定画面の名前通知。名前本体は ATT payload に収まるよう分割する。
void nk_ble_notify_name(void);
void nk_ble_notify_name_result(uint8_t operation, int32_t err);
void nk_ble_notify_time_result(int32_t err);
void nk_ble_notify_weather_result(int32_t err, const nk_weather_forecast_t *forecast);
void nk_ble_notify_weather_state(void);

#ifdef __cplusplus
}
#endif
