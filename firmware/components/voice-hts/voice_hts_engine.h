#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// HTS エンジン初期化（LittleFS からモデル読み込み、PSRAM に展開）
esp_err_t voice_hts_init(uint8_t model_id);

// HTS エンジンが準備完了か
bool voice_hts_is_ready(void);

// 任意テキスト（漢字・カナ混じり）を 16kHz モノラル PCM に合成
// 成功時: *out_pcm に PSRAM 確保された int16 配列ポインタを格納（呼び出し側が heap_caps_free() で解放）
// *out_sample_count にサンプル数を格納
esp_err_t voice_hts_synthesize_text(const char *text, int16_t **out_pcm, size_t *out_sample_count);

// DLS-023: ストリーミング合成コールバック型
// samples: 生成された 16kHz PCM チャンク
// sample_count: サンプル数
// total_estimated_samples: 合成予定の総サンプル数（プリバッファ閾値計算用）
// user_ctx: ユーザー定義コンテキスト
// 戻り値: true で合成継続、false で合成中断（ユーザー abort）
typedef bool (*voice_hts_stream_cb_t)(const int16_t *samples, size_t sample_count,
                                      size_t total_estimated_samples, void *user_ctx);

// DLS-023: ストリーミング合成 API（Vocoder 生成中にチャンク単位でコールバック呼出）
esp_err_t voice_hts_synthesize_text_stream(const char *text, voice_hts_stream_cb_t on_samples, void *user_ctx);

// 話速設定 (既定 70%)
void voice_hts_set_speed_percent(uint8_t speed_percent);

// HTS ロード完了コールバック
typedef void (*voice_hts_ready_cb_t)(void);
void voice_hts_set_ready_callback(voice_hts_ready_cb_t cb);

#ifdef __cplusplus
}
#endif
