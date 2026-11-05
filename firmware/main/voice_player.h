#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// LittleFS マウントおよび音声プレーヤー初期化
esp_err_t voice_player_init(void);
bool voice_player_init_finished(void);

// 日本語/英字テキスト（ひらがな最長一致）を WAV 連結で非同期発声要求
// 内部でキュー（または直列タスク）に送信
bool voice_player_say(const char *text);

// Single tracked request owned by the IMU task. Pending includes queue wait.
typedef enum { NK_SPEECH_IDLE, NK_SPEECH_PENDING, NK_SPEECH_DONE, NK_SPEECH_CANCELLED } nk_speech_status_t;
bool voice_player_say_tracked(const char *text, bool exact);
nk_speech_status_t voice_player_tracked_status(void);

// USB シリアルの `say`（DLS-159）。既存の exact 合成（play_phrase の [EXACT SYNTH]）にそのまま流し、
// フィラーの完全一致では飛ばさず必ず合成して、合成の直後に 1 行の計測ログを出す。verbose なら ids の記号列と
// 分かち書きも出す。発話中・聞き取り中・書き込み中・声が未初期化のときは受け付けず、理由を返す。
typedef enum {
    NK_SAY_OK, NK_SAY_EMPTY, NK_SAY_TOO_LONG, NK_SAY_NOT_READY, NK_SAY_LISTENING,
    NK_SAY_MODEL_WRITE, NK_SAY_BUSY, NK_SAY_QUEUE_FULL,
} nk_say_result_t;
nk_say_result_t voice_player_say_diag(const char *text, bool verbose);
const char *voice_player_say_result_text(nk_say_result_t result);

// Exclusive ownership while the shared I2S port is used by the microphone.
// Acquisition succeeds only with no queued/dequeued/playing voice or cue.
bool voice_player_listen_acquire(void);
void voice_player_listen_release(void);
// stt32 DLS-1082: 起動語の待受用。持っている間も発話はキューに残り（拒否しない）、待受側が yield_requested か speech_waiting を
// 見て listen_release で譲る。cue も譲りを要求して有界で待つ。聞き取り（acquire）の持ち主の扱いは変えない。
bool voice_player_listen_acquire_standby(void);
bool voice_player_listen_yield_requested(void);
bool voice_player_speech_waiting(void);
// アイドル時にだけモデル書き込み用に音声入出力を占有する。
bool voice_player_model_write_acquire(void);
void voice_player_model_write_release(void);
// Atomically return ownership and enqueue the exact tracked reply.
bool voice_player_listen_reply(const char *text);

// Audio cues use the same short lock around their Speaker calls. A listen
// owner is allowed only through the dedicated listen cue API.
bool voice_player_audio_lock(bool listen_owner);
void voice_player_audio_unlock(void);

// 現在再生中かどうか
bool voice_player_is_playing(void);
// Playback-time PCM envelope for the LCD face (false during quiet intervals).
bool voice_player_mouth_open(void);

// 再生停止
void voice_player_stop(void);

// DLS-021: スピーカー音量設定 (0〜255)
void voice_player_set_volume(uint8_t volume);
uint8_t voice_player_get_volume(void);

// DLS-027: 量子化別音声再生テスト (0=16bit PCM, 1=8bit PCM, 2=4bit IMA-ADPCM, text=発話文字列(NULLで既定「うん、そうだね」))
void voice_player_play_test_quant(uint8_t quant_mode, const char *text);

// DLS-026: HTS を通さずモーラ WAV 連結のみで再生テスト
void voice_player_say_wav_only(const char *text);

#ifdef __cplusplus
}
#endif
