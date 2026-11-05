#pragma once
// voice_hts_* の置き換え。sanoTTS-jp（漢字 -> ids -> ストリーム合成）を呼ぶ。
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sano_voice_init(void);
bool sano_voice_is_ready(void);
typedef struct {
    int dec_head, g_excite, phrase_rule_sbv2, tail_pause, int8_act;
} sano_model_flags_t;
// 合成コードと同じ翻訳単位のビルド定義を返す。
sano_model_flags_t sano_voice_model_flags(void);

// 声ごとの値（DLS-157）。model 領域の末尾の記録（voice_params.h）と、k を NVS で上書きした値。
// 記録が無い・モデルの SHA-256 が合わない・壊れているときは既定（k=1 = 整形なし / vnoise=0.03 = コアの既定）。
typedef struct {
    bool from_record;   // 記録を使っている（false = 既定）
    bool k_from_nvs;    // k は NVS の保存値
    double k, center_hz, lo_hz, hi_hz, tau_ms;
    float vnoise;
} sano_voice_params_t;
sano_voice_params_t sano_voice_params(void);   // 今効いている値（排他つきで 1 回コピー）
const char *sano_voice_model_name(void);
const char *sano_voice_model_sha8(void);
// k を変えて NVS にモデルの SHA-256 と組で保存する。範囲 0.5〜3.0。ESP_ERR_INVALID_STATE = 記録が無い / 中心が無い。
esp_err_t sano_voice_set_k(double k);
esp_err_t sano_voice_reset_k(void);            // 記録に書いた k に戻す（NVS の保存を消す）

// samples は 22.05 kHz の int16 チャンク（最大 512 サンプル）。false を返すと中断。
// total_estimated_samples は文全体を 1 回で合成するときの総サンプル数。区間に分けたときは (size_t)-1。
typedef bool (*sano_voice_stream_cb_t)(const int16_t *samples, size_t sample_count,
                                       size_t total_estimated_samples, void *user_ctx);
esp_err_t sano_voice_synthesize_text_stream(const char *text, sano_voice_stream_cb_t on_samples, void *user_ctx);

// 1 回で合成できない長さの文は「。」「！」「？」で区間に分けて合成する。
// 2 つ目以降の区間の最初の pull が済んで、まだその音を on_samples に渡す前に呼ぶ（区間の間の無音を入れる場所）。
typedef void (*sano_voice_boundary_cb_t)(void *user_ctx);
void sano_voice_set_boundary_cb(sano_voice_boundary_cb_t cb);
// 境目の無音の長さ（|x| <= 500 の連続）。前の区間の末尾 / 次の区間の先頭
typedef struct { int32_t tail_samples, lead_samples; } sano_boundary_info_t;
sano_boundary_info_t sano_boundary_info(void);

// 直近の sano_voice_synthesize_text_stream 1 本（= 1 発話）の計測（シリアルの `say` のログ用。DLS-159）。
// 数え方: sano_voice.c の「発話ごとの計測」のコメント（sanoTTS-jp-2 の main.c の synth_once と同じ）。
typedef struct {
    int route;                 // saan_g2p_classify の分類: 0 = かな / 1 = 辞書 / 2 = 拒否（G2P 自体は常に辞書経路）
    int g2p_status;            // 最後の G2P の saan_kanji_status（0 = OK）
    char g2p_status_text[40];
    int tokens;                // 最後の G2P の形態素数
    int ids;                   // 合成した ids の合計（区間に分けたときは区間の和）
    int segments;              // 合成した区間の数
    uint32_t g2p_us;           // G2P の時間の合計（1 回で合成できず分割したときは、分割前の試行を含む）
    uint32_t init_us;          // saan_stream_init の時間の合計
    uint32_t pull_us;          // saan_stream_pull_ptr の時間の合計
    uint32_t first_pull_us;    // 各区間の初回 pull の合計（warmup を含む）
    int pulls, over_budget;    // pull の回数 / 予算超過（dt > その pull の音声長）の回数
    int full_pulls;            // 満チャンク（n == SAAN_CHUNK）で各区間の初回でない pull の回数（控えの範囲）
    uint32_t full_med_us, full_mean_us;   // その中央値 / 平均
    uint32_t chunk_us;         // 満チャンク 1 個の音声長
    int pull_logged;           // 控えた pull の数（上限 256。超えた分は中央値に入らない）
    uint32_t samples;          // 合成した PCM のサンプル数（22,050 Hz）
} sano_utt_stats_t;
sano_utt_stats_t sano_voice_last_stats(void);
// true の間、合成の前に ids の記号列と辞書経路の分かち書きをログに出す（`say -v`）。nk_voice タスクの中で切り替える。
void sano_voice_set_verbose(bool on);

typedef void (*sano_voice_ready_cb_t)(void);
void sano_voice_set_ready_callback(sano_voice_ready_cb_t cb);
// 起動の段階を知らせる（label, 進み具合 0〜100 %）。sano_voice_init の中から呼ぶ
typedef void (*sano_voice_progress_cb_t)(const char *label, int pct);
void sano_voice_set_progress_cb(sano_voice_progress_cb_t cb);

#ifdef __cplusplus
}
#endif
