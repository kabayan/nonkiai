#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "motion_detector.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NK_VOICE_TEXT_MAX_LEN 64
#define NK_VOICE_NAME_MAX_BYTES 30
#define NK_VOICE_NAME_MAX_CHARS 10
#define NK_VOICE_NAME_MAX_LEN (NK_VOICE_NAME_MAX_BYTES + 1)

// 各アクションの設定情報構造体
typedef struct {
    nk_motion_event_t event;
    const char *key_name;       // 識別キー (例: "tap1", "nod")
    char text[NK_VOICE_TEXT_MAX_LEN]; // 発声テキスト (UTF-8 日本語ひらがな等)
} nk_action_voice_config_t;

// 音声設定モジュールの初期化 (NVS からロード、未設定なら tap1 は自己紹介)
esp_err_t nk_voice_config_init(void);

// アクションに対する発声テキストを取得。tap1 の標準自己紹介は10種から抽選。
// tap2 の標準設定は100種のトリビア全文（最大127バイト）を返す。
// rock / wakeup の標準設定はそれぞれ10種の入眠・起床メッセージから抽選。
// nod / tilt_lr / turn_lr の標準設定は各10文から直前を避けて抽選。
// IMUイベント生成タスク専用。返された文は次の呼び出し前にコピーすること。
const char *nk_voice_config_get_text(nk_motion_event_t event);

// Raw toy direction setting (0..3); empty means silent, nullptr means invalid.
const char *nk_voice_config_get_listen_reply(uint8_t intent);

// A random built-in name greeting, copied to caller storage for a listen reply.
// Uses the current name even when tap1 has custom text.
bool nk_voice_config_build_greeting(char *buffer, size_t size);

// 睡眠中に何らかのアクションを検出した際の寝言を10種から抽選する。
const char *nk_voice_config_get_sleep_talk(void);

// アクションの発声テキストを更新
esp_err_t nk_voice_config_set_text(nk_motion_event_t event, const char *text);

// アクションキー名から発声テキストを更新
esp_err_t nk_voice_config_set_text_by_key(const char *key, const char *text);

// 全設定の数を取得
size_t nk_voice_config_count(void);

// インデックスで設定を取得
const nk_action_voice_config_t *nk_voice_config_get_at(size_t index);

// 現在の設定を NVS に保存
esp_err_t nk_voice_config_save(void);

// 音量だけを NVS に保存（編集中の他の設定は保存しない）
esp_err_t nk_voice_config_save_volume(void);

// NVS から再読み込み
esp_err_t nk_voice_config_load(void);

// デフォルト設定にリセット
void nk_voice_config_reset_defaults(void);

// JSON 形式で全設定をエクスポート
// buffer に JSON 文字列 (例: {"tap1":"タップいち","tap2":"タップに",...}) を書き込む
esp_err_t nk_voice_config_to_json(char *buffer, size_t max_len);

// JSON 形式から設定をインポート
esp_err_t nk_voice_config_from_json(const char *json_str);

// DLS-021: 音量設定 (0〜255)
uint8_t nk_voice_config_get_volume(void);
void nk_voice_config_set_volume(uint8_t volume);

uint8_t nk_voice_config_get_display_style(void);
void nk_voice_config_set_display_style(uint8_t style);

// 現在の取り付け向けに nod / tilt_lr の最終割り当てを交換する。
bool nk_voice_config_get_swap_nod_tilt(void);
void nk_voice_config_set_swap_nod_tilt(bool enabled);

// stt32 DLS-1082: 認識モード（0=ぬいぐるみ、既定 / 1=おもちゃ）。聞き取る意図の集合と IR 送信の有無を決める。
// BLE・聞き取り・sampler の各タスクから読む。範囲外の番号は set が false を返して変えない。
uint8_t nk_voice_config_get_listen_mode(void);
bool nk_voice_config_set_listen_mode(uint8_t mode);
// Shared BLE/serial mutation: save, notify and emit mode.
bool nk_voice_config_change_listen_mode(uint8_t mode);

// IR destination: 0=built-in GPIO47 (default), 1=Grove GPIO2.
uint8_t nk_voice_config_get_ir_pin(void);
bool nk_voice_config_set_ir_pin(uint8_t pin);
bool nk_voice_config_change_ir_pin(uint8_t pin);

// ぬいぐるみの名前。名前は音声設定とは独立して NVS に保持する。
const char *nk_voice_config_get_name(void);
esp_err_t nk_voice_config_set_name(const char *name);

// 名前を自己紹介へ反映して音声設定と一緒に NVS へ保存する。
esp_err_t nk_voice_config_save_name(void);

#ifdef __cplusplus
}
#endif
