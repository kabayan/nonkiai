// voice_hts_* の置き換えの最小つなぎ。参考: sanoTTS-jp-2/esp32/main/main.c の synth_once / speak_kanji / tts_task
#include "sano_voice.h"
#include "sano_digits.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "saanotts.h"
#include "saanotts_stream.h"
#include "saan_dict.h"
#include "saan_dualcore.h"
#include "saan_kanji.h"
#include "saan_model.h"
#include "saan_pcm.h"
#include "sano_model_copy.h"
#include "sano_vocab.h"
#include "g2p.h"
#include "voice_params.h"

static const char *TAG = "sano_voice";

#ifndef SAAN_ARENA_BYTES
#error "SAAN_ARENA_BYTES must be defined"
#endif
#ifndef SAAN_S_V
#define SAAN_S_V 1.2187f
#endif

// アプリ記述の直後に残す USB 書き込み器用の目印。無効な未定義フラグは 0 とする。
#ifndef SAAN_G_EXCITE
#define SAAN_G_EXCITE 0
#endif
#ifndef SAAN_PHRASE_RULE_SBV2
#define SAAN_PHRASE_RULE_SBV2 0
#endif
#ifndef SAAN_TAIL_PAUSE
#define SAAN_TAIL_PAUSE 0
#endif
#ifndef SAAN_INT8_ACT
#define SAAN_INT8_ACT 0
#endif
#define NK_FLAGS_TEXT_(v) #v
#define NK_FLAGS_TEXT(v) NK_FLAGS_TEXT_(v)
__attribute__((section(".rodata_custom_desc"), used))
const char nk_app_model_flags[] = "NKFLAGS1" NK_FLAGS_TEXT(SAAN_DEC_HEAD) ","
    NK_FLAGS_TEXT(SAAN_G_EXCITE) "," NK_FLAGS_TEXT(SAAN_PHRASE_RULE_SBV2) ","
    NK_FLAGS_TEXT(SAAN_TAIL_PAUSE) "," NK_FLAGS_TEXT(SAAN_INT8_ACT);
_Static_assert(sizeof(nk_app_model_flags) <= 64, "フラグの目印が長すぎます");

#define SANO_MAX_IDS 350   // 1 回で合成できる ids の上限。超えると。で分割する
#define SANO_IDS_CAP (2 * 128 + 3)
#define SANO_OUT_SAMPLES 512
#define SANO_SEG_BYTES 512  // 分割した 1 区間の UTF-8 の上限（NUL を含む）

static uint8_t *g_arena;   // PSRAM（起動時に確保。16 B 境界）
static int32_t g_ids[SANO_IDS_CAP];
static saan_weights g_w;
static jdict_t g_dict, g_udict;
static bool g_ready, g_udict_ok;
static sano_voice_ready_cb_t g_ready_cb;

static sano_voice_progress_cb_t g_progress_cb;
#define PROGRESS(label, pct) do { if (g_progress_cb) g_progress_cb((label), (pct)); } while (0)

// ---- 声ごとの値（DLS-157）--------------------------------------------------------------------------------
// model 領域の末尾 4 KB セクタの記録（voice_params.h）を起動時に 1 回読む。記録のモデル SHA-256 が、いま PSRAM に
// 写したモデルの SHA-256 と一致するときだけ使う（違えば既定 = k=1 / vnoise はコアの既定）。k は NVS の
// 保存値（モデルの SHA-256 と組）で上書きできる。値は 1 つの構造体に持ち、portMUX で 1 回コピーしてから使う。
#define NVS_NS "nk_voice"
#define NVS_KEY_K "f0k"
static sano_voice_params_t g_params = { .k = 1.0, .tau_ms = NK_VP_DEFAULT_TAU_MS, .vnoise = NK_VP_DEFAULT_VNOISE };
static double g_record_k = 1.0;      // 記録に書いてあった k（reset 用）
static char g_model_name[32];
static char g_model_sha8[9];
const char *sano_voice_model_name(void) { return g_model_name[0] ? g_model_name : NULL; }
const char *sano_voice_model_sha8(void) { return g_model_sha8[0] ? g_model_sha8 : NULL; }
static uint8_t g_model_sha[32];      // 記録と一致したモデルの SHA-256（from_record のとき有効）
static portMUX_TYPE g_params_mux = portMUX_INITIALIZER_UNLOCKED;

sano_voice_params_t sano_voice_params(void) {
    sano_voice_params_t p;
    portENTER_CRITICAL(&g_params_mux);
    p = g_params;
    portEXIT_CRITICAL(&g_params_mux);
    return p;
}

static void params_log(const char *when) {
    const sano_voice_params_t p = sano_voice_params();
    if (!p.from_record) {
        ESP_LOGI(TAG, "声の値 %s: 既定（k=1 整形なし / vnoise=%.2f = コアの既定）", when, (double)p.vnoise);
        return;
    }
    ESP_LOGI(TAG, "声の値 %s: 記録を使う k=%.4f（%s）中心 %.4f Hz / 範囲 [%.4f, %.4f] Hz / τ %.1f ms / vnoise %.4f", when,
             p.k, p.k_from_nvs ? "NVS の保存値" : "記録の値", p.center_hz, p.lo_hz, p.hi_hz, p.tau_ms, (double)p.vnoise);
}

static bool nvs_k_get(double *k) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t buf[40];
    size_t n = sizeof buf;
    bool ok = nvs_get_blob(h, NVS_KEY_K, buf, &n) == ESP_OK && n == sizeof buf && memcmp(buf, g_model_sha, 32) == 0;
    if (ok) *k = nk_vp_getd(buf + 32);
    nvs_close(h);
    return ok;
}

static void params_load(void) {
    g_model_name[0] = g_model_sha8[0] = 0;
    size_t part = 0;
    const uint8_t *img = (const uint8_t *)sano_model_image(&part);
    if (!img || part <= NK_VP_SECTOR_BYTES) { ESP_LOGW(TAG, "声の値: model の写しが無い。既定"); return; }
    uint32_t mbytes = 0;
    uint8_t sha[32];
    nk_voice_values_t v;
    if (!nk_vp_decode(img + part - NK_VP_SECTOR_BYTES, &mbytes, sha, &v)) { params_log("起動（記録なし or 壊れている）"); return; }
    if (mbytes == 0 || mbytes > part - NK_VP_SECTOR_BYTES) { ESP_LOGW(TAG, "声の値: 記録のモデルの大きさ %u B が範囲外。既定", (unsigned)mbytes); return; }
    uint8_t actual[32];
    if (mbedtls_sha256(img, mbytes, actual, 0) != 0) { ESP_LOGE(TAG, "声の値: SHA-256 を計算できない。既定"); return; }
    if (memcmp(actual, sha, 32) != 0) { ESP_LOGW(TAG, "声の値: 記録のモデル SHA-256 がいまのモデルと違う。既定（記録は古い）"); return; }
    memcpy(g_model_sha, sha, 32);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 4; ++i) {
        g_model_sha8[2*i] = hex[sha[i] >> 4];
        g_model_sha8[2*i+1] = hex[sha[i] & 15];
    }
    g_model_sha8[8] = 0;
    nk_vn_decode(img + part - NK_VP_SECTOR_BYTES + NK_VP_RECORD_BYTES, g_model_name);
    sano_voice_params_t p = { .from_record = true, .k = v.k, .center_hz = v.center_hz, .lo_hz = v.lo_hz,
                              .hi_hz = v.hi_hz, .tau_ms = v.tau_ms, .vnoise = v.vnoise };
    g_record_k = v.k;
    double nk = 0.0;
    if (v.center_hz > 0.0 && nvs_k_get(&nk)) {
        if (nk >= NK_VP_K_MIN && nk <= NK_VP_K_MAX) { p.k = nk; p.k_from_nvs = true; }
        else ESP_LOGW(TAG, "声の値: NVS の k=%.3f は範囲外。記録の値を使う", nk);
    }
    portENTER_CRITICAL(&g_params_mux);
    g_params = p;
    portEXIT_CRITICAL(&g_params_mux);
    params_log("起動");
}

static esp_err_t nvs_k_store(const double *k /* NULL = 消す */) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    if (k) {
        uint8_t buf[40];
        memcpy(buf, g_model_sha, 32);
        nk_vp_putd(buf + 32, *k);
        e = nvs_set_blob(h, NVS_KEY_K, buf, sizeof buf);
    } else {
        e = nvs_erase_key(h, NVS_KEY_K);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t sano_voice_set_k(double k) {
    if (!(k >= NK_VP_K_MIN && k <= NK_VP_K_MAX)) return ESP_ERR_INVALID_ARG;
    k = (double)(long)(k * 1000.0 + 0.5) / 1000.0;
    sano_voice_params_t p = sano_voice_params();
    if (!p.from_record || !(p.center_hz > 0.0)) return ESP_ERR_INVALID_STATE;   // 中心が無い声は k を変えても効かない
    const esp_err_t e = nvs_k_store(&k);
    if (e != ESP_OK) return e;
    portENTER_CRITICAL(&g_params_mux);
    g_params.k = k;
    g_params.k_from_nvs = true;
    portEXIT_CRITICAL(&g_params_mux);
    params_log("k を変更");
    return ESP_OK;
}

esp_err_t sano_voice_reset_k(void) {
    if (!sano_voice_params().from_record) return ESP_ERR_INVALID_STATE;
    const esp_err_t e = nvs_k_store(NULL);
    if (e != ESP_OK) return e;
    portENTER_CRITICAL(&g_params_mux);
    g_params.k = g_record_k;
    g_params.k_from_nvs = false;
    portEXIT_CRITICAL(&g_params_mux);
    params_log("k を記録の値に戻した");
    return ESP_OK;
}

// 区間の saan_stream_init 直後に呼ぶ（コアは init で既定に戻す）。記録が無ければ何もしない = 今までと同じ。
static void params_apply(saan_stream *st, const sano_voice_params_t *p) {
    if (!p->from_record) return;
    const saan_f0shape f = { p->k, p->center_hz, p->lo_hz, p->hi_hz, p->tau_ms };
    saan_status r = saan_stream_set_f0shape(st, &f);
    if (r != SAAN_OK) ESP_LOGE(TAG, "F0 整形の設定に失敗: %s（整形なしで続ける）", saan_strerror(r));
    r = saan_stream_set_vnoise(st, p->vnoise);
    if (r != SAAN_OK) ESP_LOGE(TAG, "vnoise の設定に失敗: %s（既定で続ける）", saan_strerror(r));
}

esp_err_t sano_voice_init(void) {
#if SAAN_DUALCORE
    if (!saan_dualcore_start()) return ESP_FAIL;
#endif
#if defined(SAAN_HEAD_EXT) && SAAN_HEAD_EXT
    {
        const size_t hb = saan_stream_head_ext_bytes();
        float *hbuf = (float *)heap_caps_aligned_alloc(16, hb, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!hbuf) return ESP_ERR_NO_MEM;
        saan_stream_set_head_ext(hbuf, hb);
    }
#endif
    g_arena = (uint8_t *)heap_caps_aligned_alloc(16, SAAN_ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_arena) { ESP_LOGE(TAG, "arena %d B を PSRAM に確保できない", (int)SAAN_ARENA_BYTES); return ESP_ERR_NO_MEM; }
    ESP_LOGI(TAG, "arena %d B を PSRAM (%p) に確保", (int)SAAN_ARENA_BYTES, (void *)g_arena);
    if (!saan_model_open(&g_w)) return ESP_FAIL;
    PROGRESS("WEIGHTS ok", 18);
    params_load();   // フィラーのプリロード（voice_player）より前に値を決める
    if (!(saan_dict_open(&g_dict) && saan_kanji_init())) return ESP_FAIL;
    g_udict_ok = saan_userdict_open(&g_udict, &g_dict);
    PROGRESS("DICT ok", 19);
    g_ready = true;
    ESP_LOGI(TAG, "ready (arena %d B)", (int)SAAN_ARENA_BYTES);
    if (g_ready_cb) g_ready_cb();
    return ESP_OK;
}

bool sano_voice_is_ready(void) { return g_ready; }

sano_model_flags_t sano_voice_model_flags(void) {
    sano_model_flags_t flags = { .dec_head = SAAN_DEC_HEAD };
#if SAAN_G_EXCITE
    flags.g_excite = SAAN_G_EXCITE;
#endif
#if SAAN_PHRASE_RULE_SBV2
    flags.phrase_rule_sbv2 = SAAN_PHRASE_RULE_SBV2;
#endif
#if SAAN_TAIL_PAUSE
    flags.tail_pause = SAAN_TAIL_PAUSE;
#endif
#if SAAN_INT8_ACT
    flags.int8_act = SAAN_INT8_ACT;
#endif
    return flags;
}

void sano_voice_set_ready_callback(sano_voice_ready_cb_t cb) { g_ready_cb = cb; }
void sano_voice_set_progress_cb(sano_voice_progress_cb_t cb) { g_progress_cb = cb; }

static sano_voice_boundary_cb_t g_boundary_cb;
static sano_boundary_info_t g_binfo;
static int32_t g_tail_run;   // 直前までの PCM の末尾の無音（|x| <= SIL_THR の連続）

#define SANO_SIL_THR 500

sano_boundary_info_t sano_boundary_info(void) { return g_binfo; }
void sano_voice_set_boundary_cb(sano_voice_boundary_cb_t cb) { g_boundary_cb = cb; }

// ---- 発話ごとの計測（シリアルの `say` のログ用。DLS-159）-------------------------------------------------------
// 数え方は sanoTTS-jp-2 の esp32/main/main.c（synth_once）と同じ:
//   pull 1 回の時間 = saan_stream_pull_ptr の前後の esp_timer_get_time の差（再生への受け渡しを含まない）
//   定常 xRT = 満チャンク pull（n == SAAN_CHUNK、各ストリームの初回 pull を除く）の中央値 / 満チャンクの音声長
//   予算超過 = pull の dt > その pull の音声長（n * SAAN_HOP / SAAN_SR）
// 計測しているのは合成を呼んでいる側のタスク（nk_voice）だけ。1 本の sano_voice_synthesize_text_stream が 1 発話。
#define SANO_PULL_LOG 256
static sano_utt_stats_t g_st;
static uint32_t g_pull_us[SANO_PULL_LOG];
static uint8_t g_pull_n[SANO_PULL_LOG];   // bit7 = そのストリームの初回 pull
static int g_pull_logged;
static bool g_verbose;

void sano_voice_set_verbose(bool on) { g_verbose = on; }

static int cmp_u32(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

sano_utt_stats_t sano_voice_last_stats(void) {
    sano_utt_stats_t r = g_st;
    static uint32_t full[SANO_PULL_LOG];
    int nf = 0;
    uint64_t sum = 0;
    for (int i = 0; i < g_pull_logged; ++i) {
        if ((g_pull_n[i] & 0x80) || (g_pull_n[i] & 0x7f) != SAAN_CHUNK) continue;
        full[nf++] = g_pull_us[i];
        sum += g_pull_us[i];
    }
    r.full_pulls = nf;
    r.full_med_us = r.full_mean_us = 0;
    if (nf > 0) {
        qsort(full, (size_t)nf, sizeof full[0], cmp_u32);
        r.full_med_us = (nf & 1) ? full[nf / 2] : (full[nf / 2 - 1] + full[nf / 2]) / 2;
        r.full_mean_us = (uint32_t)(sum / (uint64_t)nf);
    }
    r.pull_logged = g_pull_logged;
    r.chunk_us = (uint32_t)((uint64_t)SAAN_CHUNK * SAAN_HOP * 1000000ull / SAAN_SR);
    return r;
}

static saan_kanji_status g2p(const char *t, size_t n, int32_t *n_ids) {
    int n_tok = 0;
    const int64_t t0 = esp_timer_get_time();
    const saan_kanji_status ks = saan_kanji_to_ids_ex(&g_dict, g_udict_ok ? &g_udict : NULL, t, n,
                                g_arena, SAAN_ARENA_BYTES, g_ids, SANO_IDS_CAP, n_ids, &n_tok);
    g_st.g2p_us += (uint32_t)(esp_timer_get_time() - t0);
    g_st.g2p_status = (int)ks;
    g_st.tokens = n_tok;
    snprintf(g_st.g2p_status_text, sizeof g_st.g2p_status_text, "%s", saan_kanji_strerror(ks));
    return ks;
}

// `say -v` 用: 区間ごとに、ids を記号に戻した列と、辞書経路の分かち書き（表層 / 読み / 発音 / アクセント）を出す。
// g2p の直後・saan_stream_init の前（arena が空いている間）に呼ぶ。Viterbi は arena の先頭を作業領域に使う。
static void log_verbose(const char *text, size_t n, int32_t n_ids) {
    static char sym[640];
    sano_vocab_format((const int *)g_ids, (int)n_ids, sym, sizeof sym);
    ESP_LOGI(TAG, "[SAY-V] ids %d 個（PAD の _ は省く）: %s", (int)n_ids, sym);
    uint8_t key[SAAN_KANJI_KEY_MAX];
    size_t key_n = sizeof key;
    if (n >= sizeof key || jdict_encode_key(&g_dict, (const uint8_t *)text, n, key, &key_n) != 0) {
        ESP_LOGW(TAG, "[SAY-V] 分かち書き: 鍵に符号化できない");
        return;
    }
    jdict_token_t tok[SAAN_KANJI_MAX_TOK];
    const int nt = jdict_analyze_ex(&g_dict, g_udict_ok ? &g_udict : NULL, key, key_n,
                                    g_arena, SAAN_KANJI_VITERBI_N, tok, SAAN_KANJI_MAX_TOK);
    if (nt <= 0) { ESP_LOGW(TAG, "[SAY-V] 分かち書き: 解析できない（%d）", nt); return; }
    char line[384];
    size_t len = 0;
    for (int i = 0; i < nt; ++i) {
        char surf[96], feat[SAAN_KANJI_FEAT_MAX];
        if (jdict_key_to_utf8(&g_dict, key, tok[i].begin, tok[i].end, surf, sizeof surf) < 0) snprintf(surf, sizeof surf, "?");
        int r = -1;
        const bool unk = (tok[i].entry & JDICT_UNKNOWN_FLAG) != 0;
        if (unk) {
            r = jdict_unk_guess(&g_dict, tok[i].entry, surf, feat, sizeof feat);
            if (r < 0) r = jdict_unk_feature(&g_dict, tok[i].entry, surf, feat, sizeof feat);
        } else {
            r = jdict_entry_feature_ex(&g_dict, g_udict_ok ? &g_udict : NULL, tok[i].entry, surf, feat, sizeof feat);
        }
        // feature は MeCab 形式: 品詞,品詞細分類1〜3,活用型,活用形,原形,読み,発音,アクセント型/モーラ数,結合規則...
        // 読み・発音・アクセントは 8〜10 列目（0 起算で 7〜9）。未知語は 8 列しか無い。
        char read[48] = "-", pron[48] = "-", acc[16] = "-";
        if (r >= 0) {
            int col = 0;
            const char *p = feat;
            while (*p) {
                const char *q = p;
                while (*q && *q != ',') ++q;
                const size_t w = (size_t)(q - p);
                if (col == 7) snprintf(read, sizeof read, "%.*s", (int)w, p);
                else if (col == 8) snprintf(pron, sizeof pron, "%.*s", (int)w, p);
                else if (col == 9) snprintf(acc, sizeof acc, "%.*s", (int)w, p);
                ++col;
                if (!*q) break;
                p = q + 1;
            }
        }
        int w = snprintf(line + len, sizeof line - len, "%s%s/%s/%s/%s%s", i ? " | " : "", surf, read, pron, acc, unk ? "(未知語)" : "");
        if (w < 0 || (size_t)w >= sizeof line - len) { snprintf(line + sizeof line - 5, 5, "..."); break; }
        len += (size_t)w;
    }
    ESP_LOGI(TAG, "[SAY-V] 分かち書き %d 語（表層/読み/発音/アクセント型）: %s", nt, line);
}


// 文の終わり（。！？）の直後までを 1 区間として end を返す。無ければ末尾
static const char *sentence_end(const char *b) {
    for (const char *p = b; *p; ++p) {
        if (strncmp(p, "。", 3) == 0 || strncmp(p, "！", 3) == 0 || strncmp(p, "？", 3) == 0) return p + 3;
    }
    return b + strlen(b);
}

static esp_err_t synthesize_normalized(const char *text, sano_voice_stream_cb_t on_samples, void *user_ctx) {
    if (!g_ready || !text || !on_samples) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = ESP_OK;
    g_tail_run = 0;
    const sano_voice_params_t vp = sano_voice_params();   // 1 本の発話の中では同じ値を使う
    memset(&g_st, 0, sizeof g_st);
    g_pull_logged = 0;
    {
        // 分類: かな中間表現 / 辞書経路 / 拒否（csrc/g2p.h）。⚠️ この firmware の G2P は分類に関わらず
        // 常に saan_kanji_to_ids_ex（辞書経路）を通る。ここは入力の種類を知らせるだけ。
        saan_g2p_status why; int32_t eb;
        g_st.route = (int)saan_g2p_classify(text, strlen(text), &why, &eb);
    }

    // 文全体を 1 回で合成できるか
    int32_t n_ids = 0;
    saan_kanji_status ks = g2p(text, strlen(text), &n_ids);
    const bool whole_ok = (ks == SAAN_KANJI_OK && n_ids <= SANO_MAX_IDS);
    if (!whole_ok && ks != SAAN_KANJI_OK && ks != SAAN_KANJI_ERR_TOO_LONG && ks != SAAN_KANJI_ERR_IDS) {
        ESP_LOGE(TAG, "G2P: %s", saan_kanji_strerror(ks));
        return ESP_FAIL;
    }
    if (whole_ok && g_verbose) log_verbose(text, strlen(text), n_ids);
    if (!whole_ok) ESP_LOGW(TAG, "1 回で合成できない（G2P=%s / ids=%d / 上限 %d）。。で分割する",
                            saan_kanji_strerror(ks), (int)n_ids, SANO_MAX_IDS);
    const char *b = text;
    for (int k = 0; *b; ++k) {
        const char *e = whole_ok ? b + strlen(b) : sentence_end(b);
        if (!(k == 0 && whole_ok)) {
            // G2P は文末の判定（疑問文・句点）に strlen を使うので、区間を NUL 終端した写しで渡す
            static char seg[SANO_SEG_BYTES];
            const size_t len = (size_t)(e - b);
            if (len >= sizeof seg) {
                ESP_LOGE(TAG, "区間 %d が長すぎる（%u B / 上限 %u B）", k + 1, (unsigned)len, (unsigned)(sizeof seg - 1));
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(seg, b, len);
            seg[len] = '\0';
            ks = g2p(seg, len, &n_ids);
            if (ks == SAAN_KANJI_OK && g_verbose) log_verbose(seg, len, n_ids);
            if (ks != SAAN_KANJI_OK || n_ids > SANO_MAX_IDS) {
                ESP_LOGE(TAG, "区間 %d が合成できない（G2P=%s / ids=%d / 上限 %d）", k + 1,
                         saan_kanji_strerror(ks), (int)n_ids, SANO_MAX_IDS);
                return ESP_ERR_INVALID_SIZE;
            }
        }
        saan_arena a;
        saan_arena_init(&a, g_arena, SAAN_ARENA_BYTES);
        saan_stream st;
        const int64_t t_init = esp_timer_get_time();
        if (saan_stream_init(&st, &g_w, &a, g_ids, n_ids, (float)SAAN_S_V) != SAAN_OK) return ESP_FAIL;
        g_st.init_us += (uint32_t)(esp_timer_get_time() - t_init);
        g_st.ids += n_ids;
        g_st.segments++;
        bool first_pull = true;
        params_apply(&st, &vp);
        const size_t total = whole_ok ? (size_t)st.n_frames * SAAN_HOP : (size_t)-1;
        int16_t buf[SANO_OUT_SAMPLES];
        bool seg_first = true;
        for (;;) {
            const float *chunk = NULL;
            int32_t n = 0;
            const int64_t t_pull = esp_timer_get_time();
            saan_status s = saan_stream_pull_ptr(&st, &chunk, &n);
            const uint32_t dt = (uint32_t)(esp_timer_get_time() - t_pull);
            if (s != SAAN_OK) return ESP_FAIL;
            if (n <= 0) break;
            g_st.pull_us += dt;
            g_st.pulls++;
            g_st.samples += (uint32_t)n * SAAN_HOP;
            if (first_pull) g_st.first_pull_us += dt;
            if (dt > (uint32_t)((uint64_t)n * SAAN_HOP * 1000000ull / SAAN_SR)) g_st.over_budget++;
            if (g_pull_logged < SANO_PULL_LOG) {
                g_pull_us[g_pull_logged] = dt;
                g_pull_n[g_pull_logged] = (uint8_t)n | (first_pull ? 0x80 : 0);
                g_pull_logged++;
            }
            first_pull = false;
            const size_t ns = (size_t)n * SAAN_HOP;
            if (seg_first && k > 0) {
                // 前の区間の末尾 / この区間の先頭の無音を知らせてから、間の無音を入れさせる
                g_binfo.tail_samples = g_tail_run;
                int32_t lead = 0;
                for (size_t i = 0; i < ns; ++i) {
                    int x = saan_f32_to_i16(chunk[i]);
                    if (x < 0) x = -x;
                    if (x > SANO_SIL_THR) break;
                    ++lead;
                }
                g_binfo.lead_samples = lead;
                if (g_boundary_cb) g_boundary_cb(user_ctx);
            }
            seg_first = false;
            for (size_t off = 0; off < ns; off += SANO_OUT_SAMPLES) {
                const size_t m = ns - off < SANO_OUT_SAMPLES ? ns - off : SANO_OUT_SAMPLES;
                for (size_t i = 0; i < m; ++i) {
                    buf[i] = saan_f32_to_i16(chunk[off + i]);
                    const int x = buf[i] < 0 ? -buf[i] : buf[i];
                    g_tail_run = (x <= SANO_SIL_THR) ? g_tail_run + 1 : 0;
                }
                if (!on_samples(buf, m, total, user_ctx)) return ESP_ERR_INVALID_STATE;
            }
        }
        b = e;
    }
    return ret;
}

// DLS-180: 分類・全文・分割の入口を全角数字にそろえる。
esp_err_t sano_voice_synthesize_text_stream(const char *text, sano_voice_stream_cb_t on_samples, void *user_ctx) {
    if (!g_ready || !text || !on_samples) return ESP_ERR_INVALID_STATE;
    const size_t len = strlen(text);
    if (len > (SIZE_MAX - 1) / 3) return ESP_ERR_INVALID_SIZE;
    const size_t capacity = len * 3 + 1;
    char *normalized = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!normalized) return ESP_ERR_NO_MEM;
    esp_err_t ret = ESP_ERR_INVALID_SIZE;
    if (sano_normalize_digits(text, normalized, capacity)) {
        ret = synthesize_normalized(normalized, on_samples, user_ctx);
    }
    free(normalized);
    return ret;
}
