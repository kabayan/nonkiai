#include "voice_hts_engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include <sstream>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "HTS_engine.h"
#include "hts_mlpg_T.h"
#include "hts_param_extract.h"
#include "hts_synth_T.h"
#include "front_synth.h"

namespace {

constexpr char kTag[] = "nk-hts";
constexpr const char *kVoicePaths[] = {
    "/littlefs/htsvoice/female1.htsvoice",
    "/littlefs/htsvoice/man2.htsvoice",
};
constexpr char kTriePath[]   = "/littlefs/htsvoice/tier1_infl_trie_fw.bin";
constexpr char kAccCtxPath[] = "/littlefs/htsvoice/tier1_acc_ctx.tsv";
constexpr char kKfbPath[]    = "/littlefs/htsvoice/tier1_kanji_fallback.tsv";
constexpr char kDigitPath[]  = "/littlefs/htsvoice/tier1_digit.tsv";
constexpr char kCounterPath[]= "/littlefs/htsvoice/tier1_counter.tsv";

constexpr size_t kPstreamReserve = 128 * 1024;

static HTS_Engine s_engine;
static volatile bool s_engine_loaded = false;
static volatile bool s_load_started = false;
static const char *s_voice_path = kVoicePaths[1];
static uint32_t s_speed_percent = 70;
static voice_hts_ready_cb_t s_ready_cb = nullptr;

static trie::Trie g_trie;
static uint8_t *g_trie_blob = nullptr;
static front::CtxTable g_acc_ctx;
static front::KFTable g_kanji_fb;
static front::NodeTable g_digit;
static front::NodeTable g_counter;
static bool g_front_loaded = false;

static int16_t clamp_pcm(double sample) {
    if (sample > 32767.0) return 32767;
    if (sample < -32768.0) return -32768;
    return static_cast<int16_t>(sample >= 0.0 ? sample + 0.5 : sample - 0.5);
}

static size_t matrix_bytes(size_t rows, size_t columns) {
    return rows * sizeof(double *) + rows * columns * sizeof(double) + rows * 32;
}

static size_t matrix_bytes_float(size_t rows, size_t columns) {
    return rows * sizeof(void *) + rows * columns * sizeof(float) + rows * 48;
}

static size_t estimate_pstream_bytes(HTS_Engine *engine) {
    HTS_SStreamSet *sss = &engine->sss;
    size_t total = sizeof(HTS_PStream) * sss->nstream;
    for (size_t i = 0; i < sss->nstream; ++i) {
        HTS_SStream *stream = &sss->sstream[i];
        size_t length = sss->total_frame;
        if (stream->msd != nullptr) {
            length = 0;
            for (size_t state = 0; state < sss->total_state; ++state) {
                if (stream->msd[state] > engine->condition.msd_threshold[i]) length += sss->duration[state];
            }
            total += sss->total_frame * sizeof(HTS_Boolean);
        }
        const size_t width = stream->win_max_width * 2 + 1;
        if (length > 0) {
            if (i == 0) {
                total += matrix_bytes_float(length, stream->vector_length * stream->win_size) * 2;
                total += length * sizeof(float) * 3;
                total += matrix_bytes_float(length, width) * 2;
                total += matrix_bytes_float(length, stream->vector_length);
                total += stream->win_size * width * sizeof(float);
            } else {
                total += matrix_bytes(length, stream->vector_length * stream->win_size) * 2;
                total += length * sizeof(double) * 3;
                total += matrix_bytes(length, width) * 2;
                total += matrix_bytes(length, stream->vector_length);
            }
        }
        total += stream->win_size * (sizeof(int) * 2 + sizeof(double *));
        for (size_t window = 0; window < stream->win_size; ++window) {
            const int left = stream->win_l_width[window];
            const int right = stream->win_r_width[window];
            total += static_cast<size_t>(left + right == 0 ? -2 * left + 1 : -2 * left) * sizeof(double);
        }
        if (stream->gv_mean != nullptr) total += stream->vector_length * sizeof(double) * 2 + length;
    }
    return total;
}

// --- 文頭・文末 sil トリム -------------------------------------------------
// HTS の合成音声は文頭・文末に長い無音 (sil) を含む (実測: 文頭 ~0.9s, 文末 ~1.25s)。
// パラメータ生成 (MLPG) より前に、HTS_SStreamSet の状態継続長 (duration[]) を
// 縮めることで無音を短くする。後段の HTS_PStreamSet_create / float MLPG の
// load_from_sss はいずれも HTS_SStreamSet_get_duration() / get_total_frame() 経由で
// この配列を読むだけなので、ここで書き換えれば以降の処理は変更不要。
constexpr uint32_t kSilLeadTargetMs = 50;   // 文頭 sil の目標合計長 (ms)
constexpr uint32_t kSilTailTargetMs = 100;  // 文末 sil の目標合計長 (ms)

static bool s_sil_trim_label_format_logged = false;

// full-context ラベル文字列から中心音素を取り出す (最初の '-' と、その次の '+' の間)。
// 例: "xx^xx-sil+k=o/..." -> "sil"
static bool extract_center_phone(const std::string &label, std::string *out) {
    size_t dash = label.find('-');
    if (dash == std::string::npos) return false;
    size_t plus = label.find('+', dash + 1);
    if (plus == std::string::npos) return false;
    *out = label.substr(dash + 1, plus - dash - 1);
    return true;
}

// 目標ミリ秒をフレーム数に換算する (target_ms * サンプリング周波数 / 1000 / fperiod)。
static size_t sil_target_frames(HTS_Engine *engine, uint32_t target_ms) {
    size_t sr = HTS_Engine_get_sampling_frequency(engine);
    size_t fperiod = HTS_Engine_get_fperiod(engine);
    if (fperiod == 0) return 0;
    return static_cast<size_t>((static_cast<uint64_t>(target_ms) * sr) / 1000ULL / fperiod);
}

// duration[0..nstate) を、合計が target_frames にちょうど一致するよう、元の長さに
// 比例して配分し直す (各状態最低 1 フレーム)。呼び出し前提: orig_sum > target_frames
// かつ target_frames >= nstate (それ以外は呼び出し側で先に処理する)。
static void distribute_target_frames(size_t *duration, size_t nstate, size_t orig_sum, size_t target_frames) {
    std::vector<double> ideal(nstate);
    std::vector<size_t> alloc(nstate);
    for (size_t i = 0; i < nstate; ++i) {
        ideal[i] = static_cast<double>(duration[i]) * static_cast<double>(target_frames) / static_cast<double>(orig_sum);
        size_t base = static_cast<size_t>(ideal[i]); // floor (ideal >= 0)
        alloc[i] = base < 1 ? 1 : base;
    }
    long long sum_alloc = 0;
    for (size_t i = 0; i < nstate; ++i) sum_alloc += static_cast<long long>(alloc[i]);
    long long diff = static_cast<long long>(target_frames) - sum_alloc;

    std::vector<size_t> order(nstate);
    for (size_t i = 0; i < nstate; ++i) order[i] = i;

    if (diff > 0) {
        // 端数 (ideal[i] - floor(ideal[i])) が大きい状態から 1 フレームずつ積み増す。
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return (ideal[a] - std::floor(ideal[a])) > (ideal[b] - std::floor(ideal[b]));
        });
        size_t idx = 0;
        while (diff > 0) {
            alloc[order[idx % nstate]] += 1;
            ++idx;
            --diff;
        }
    } else if (diff < 0) {
        // 端数が小さい (= floor で 1 に繰り上げられた分の超過が大きい) 状態から、
        // 1 フレームより多く割り当てられているものに限って削る。
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return (ideal[a] - std::floor(ideal[a])) < (ideal[b] - std::floor(ideal[b]));
        });
        long long need = -diff;
        size_t idx = 0;
        while (need > 0) {
            size_t i = order[idx % nstate];
            if (alloc[i] > 1) {
                alloc[i] -= 1;
                --need;
            }
            ++idx;
        }
    }

    for (size_t i = 0; i < nstate; ++i) duration[i] = alloc[i];
}

// state_offset から nstate 個の状態継続長の合計が target_frames を超えていれば縮める。
// 超えていなければ何もしない (縮めるだけ)。戻り値: トリム後の合計フレーム数。
static size_t trim_state_range(HTS_SStreamSet *sss, size_t state_offset, size_t nstate, size_t target_frames) {
    if (nstate == 0) return 0;
    size_t orig_sum = 0;
    for (size_t i = 0; i < nstate; ++i) orig_sum += sss->duration[state_offset + i];
    if (orig_sum <= target_frames) return orig_sum;
    if (target_frames < nstate) {
        // 各状態を 1 フレームまで削っても目標に届かない場合は、1 フレームずつ残す。
        for (size_t i = 0; i < nstate; ++i) sss->duration[state_offset + i] = 1;
        return nstate;
    }
    distribute_target_frames(&sss->duration[state_offset], nstate, orig_sum, target_frames);
    return target_frames;
}

// 文頭・文末ラベルの中心音素が sil であれば、その継続長を縮める。
// HTS_Engine_generate_state_sequence_from_strings() 成功直後、パラメータ生成より前に呼ぶこと。
static void trim_lead_tail_sil(HTS_Engine *engine, const std::vector<std::string> &labels) {
    if (labels.empty()) return;
    HTS_SStreamSet *sss = &engine->sss;
    const size_t nstate = sss->nstate;
    const size_t total_state = sss->total_state;

    if (!s_sil_trim_label_format_logged) {
        s_sil_trim_label_format_logged = true;
        ESP_LOGI(kTag, "[SIL TRIM] label sample: head='%s' tail='%s'",
                 labels.front().substr(0, 80).c_str(), labels.back().substr(0, 80).c_str());
    }

    if (nstate == 0 || total_state != labels.size() * nstate) {
        ESP_LOGW(kTag, "[SIL TRIM] state count mismatch (total_state=%u labels=%u nstate=%u); skip trim",
                 (unsigned)total_state, (unsigned)labels.size(), (unsigned)nstate);
        return;
    }

    std::string lead_center, tail_center;
    bool lead_parsed = extract_center_phone(labels.front(), &lead_center);
    bool tail_parsed = extract_center_phone(labels.back(), &tail_center);
    if (!lead_parsed || !tail_parsed) {
        ESP_LOGW(kTag, "[SIL TRIM] unexpected label format (no '-'/'+' around center phone); skip trim");
        return;
    }

    const size_t tail_offset = total_state - nstate;
    // 1 ラベルしかない (先頭・末尾の状態範囲が重なる) 場合は末尾側を別途処理しない。
    const bool ranges_overlap = labels.size() < 2;

    size_t lead_before = 0, tail_before = 0;
    for (size_t i = 0; i < nstate; ++i) lead_before += sss->duration[i];
    for (size_t i = 0; i < nstate; ++i) tail_before += sss->duration[tail_offset + i];

    size_t lead_after = lead_before;
    size_t tail_after = tail_before;

    if (lead_center == "sil") {
        lead_after = trim_state_range(sss, 0, nstate, sil_target_frames(engine, kSilLeadTargetMs));
    }
    if (tail_center == "sil" && !ranges_overlap) {
        tail_after = trim_state_range(sss, tail_offset, nstate, sil_target_frames(engine, kSilTailTargetMs));
    }

    size_t removed = (lead_before - lead_after) + (tail_before - tail_after);
    size_t total_before = sss->total_frame;
    if (removed > 0) sss->total_frame -= removed;

    const size_t fperiod = HTS_Engine_get_fperiod(engine);
    const size_t sr = HTS_Engine_get_sampling_frequency(engine);
    auto to_ms = [&](size_t frames) -> double {
        return (sr == 0) ? 0.0 : static_cast<double>(frames) * fperiod * 1000.0 / static_cast<double>(sr);
    };

    ESP_LOGI(kTag, "[SIL TRIM] lead sil %u->%u frames (%.1f->%.1f ms), tail sil %u->%u frames (%.1f->%.1f ms), total_frame %u->%u",
             (unsigned)lead_before, (unsigned)lead_after, to_ms(lead_before), to_ms(lead_after),
             (unsigned)tail_before, (unsigned)tail_after, to_ms(tail_before), to_ms(tail_after),
             (unsigned)total_before, (unsigned)sss->total_frame);
}

static bool read_file_to_string(const char *path, std::string &out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return false;
    }
    out.resize(size);
    size_t read_bytes = fread(&out[0], 1, size, f);
    fclose(f);
    return read_bytes == static_cast<size_t>(size);
}

static bool front_load_tables() {
    if (g_front_loaded) return true;

    // 1. Trie
    FILE *tf = fopen(kTriePath, "rb");
    if (!tf) {
        ESP_LOGE(kTag, "Trie open failed: %s", kTriePath);
        return false;
    }
    fseek(tf, 0, SEEK_END);
    size_t tn = ftell(tf);
    fseek(tf, 0, SEEK_SET);
    g_trie_blob = (uint8_t *)heap_caps_malloc(tn, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_trie_blob) {
        ESP_LOGE(kTag, "Trie blob PSRAM alloc failed: %u bytes", (unsigned)tn);
        fclose(tf);
        return false;
    }
    if (fread(g_trie_blob, 1, tn, tf) != tn) {
        ESP_LOGE(kTag, "Trie read short");
        fclose(tf);
        return false;
    }
    fclose(tf);
    if (!g_trie.attach(g_trie_blob, tn)) {
        ESP_LOGE(kTag, "Trie attach failed");
        return false;
    }

    // 2. Side tables
    try {
        std::string s;
        if (read_file_to_string(kAccCtxPath, s)) {
            std::istringstream iss(s);
            g_acc_ctx = front::parse_acc_ctx(iss);
        }
        s.clear();
        if (read_file_to_string(kKfbPath, s)) {
            std::istringstream iss(s);
            g_kanji_fb = front::parse_kanji_fallback(iss);
        }
        s.clear();
        if (read_file_to_string(kDigitPath, s)) {
            std::istringstream iss(s);
            g_digit = front::parse_node_table(iss);
        }
        s.clear();
        if (read_file_to_string(kCounterPath, s)) {
            std::istringstream iss(s);
            g_counter = front::parse_node_table(iss);
        }
    } catch (const std::exception &e) {
        ESP_LOGW(kTag, "side table parse exception: %s", e.what());
    }

    ESP_LOGI(kTag, "front tables loaded: trie=%u surfs, acc_ctx=%u, kanji_fb=%u, digit=%u, counter=%u",
             (unsigned)g_trie.count(), (unsigned)g_acc_ctx.size(), (unsigned)g_kanji_fb.size(),
             (unsigned)g_digit.size(), (unsigned)g_counter.size());
    g_front_loaded = true;
    return true;
}

static void load_engine_worker(void *arg) {
    int64_t t0 = esp_timer_get_time();
    // DLS-032/DLS-033: Core 1 常駐 MLPG ワーカーは、内部 SRAM に余裕があるモデルロード前に
    // 1 回だけ生成する。生成可否と直後の内部 SRAM 状況は init_resident_worker() 内でログする。
    hts_tmpl::MlpgT<float>::init_resident_worker();
    HTS_Engine_initialize(&s_engine);
    char *voices[] = {const_cast<char *>(s_voice_path)};
    if (HTS_Engine_load(&s_engine, voices, 1) != TRUE) {
        HTS_Engine_clear(&s_engine);
        ESP_LOGE(kTag, "HTS model load failed: %s", s_voice_path);
        vTaskDelete(nullptr);
        return;
    }
    if (HTS_Engine_get_sampling_frequency(&s_engine) != 16000) {
        ESP_LOGE(kTag, "unsupported model rate=%u", (unsigned)HTS_Engine_get_sampling_frequency(&s_engine));
        HTS_Engine_clear(&s_engine);
        vTaskDelete(nullptr);
        return;
    }
    if (!front_load_tables()) {
        ESP_LOGE(kTag, "front load tables failed");
        HTS_Engine_clear(&s_engine);
        vTaskDelete(nullptr);
        return;
    }

    s_engine_loaded = true;
    ESP_LOGI(kTag, "HTS engine ready model=%s (%lld ms): psram_free=%u", s_voice_path,
             (esp_timer_get_time() - t0) / 1000,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (s_ready_cb != nullptr) {
        s_ready_cb();
    }
    vTaskDelete(nullptr);
}

} // namespace

extern "C" {

void voice_hts_set_ready_callback(voice_hts_ready_cb_t cb) {
    s_ready_cb = cb;
    if (s_engine_loaded && s_ready_cb != nullptr) {
        s_ready_cb();
    }
}

esp_err_t voice_hts_init(uint8_t model_id) {
    if (model_id >= sizeof(kVoicePaths) / sizeof(kVoicePaths[0])) return ESP_ERR_INVALID_ARG;
    if (s_load_started) return ESP_OK;
    s_voice_path = kVoicePaths[model_id];
    s_load_started = true;

    // notif DLS-279: HTS モデルロードおよび辞書パースはコールスタックを深く消費するため、
    // 独立した 32KB スタックタスクで非同期にロードする。
    BaseType_t ret = xTaskCreatePinnedToCore(
        load_engine_worker, "hts_load", 32768, nullptr, 1, nullptr, 1);
    if (ret != pdPASS) {
        ESP_LOGE(kTag, "failed to create hts_load task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool voice_hts_is_ready(void) {
    return s_engine_loaded;
}

void voice_hts_set_speed_percent(uint8_t speed_percent) {
    s_speed_percent = std::clamp<uint8_t>(speed_percent, 30, 200);
}

// DLS-023: ストリーミング合成 API
esp_err_t voice_hts_synthesize_text_stream(const char *text, voice_hts_stream_cb_t on_samples, void *user_ctx) {
    ESP_RETURN_ON_FALSE(s_engine_loaded, ESP_ERR_INVALID_STATE, kTag, "HTS not initialized");
    ESP_RETURN_ON_FALSE(text != nullptr && *text != '\0', ESP_ERR_INVALID_ARG, kTag, "empty text");
    ESP_RETURN_ON_FALSE(on_samples != nullptr, ESP_ERR_INVALID_ARG, kTag, "null callback");

    int64_t t_start = esp_timer_get_time();

    // 1. フロントエンドでラベル列生成
    std::vector<std::string> labels =
        front::front_synthesize_labels(text, g_trie, g_acc_ctx, g_kanji_fb, g_digit, g_counter);
    int64_t t_front_done = esp_timer_get_time();
    if (labels.empty()) {
        ESP_LOGW(kTag, "front synthesized 0 labels for '%s'", text);
        return ESP_FAIL;
    }

    std::vector<char *> lp;
    lp.reserve(labels.size());
    for (auto &l : labels) lp.push_back(const_cast<char *>(l.c_str()));

    // 2. 状態系列生成
    HTS_Engine_set_speed(&s_engine, static_cast<double>(s_speed_percent) / 100.0);
    if (HTS_Engine_generate_state_sequence_from_strings(&s_engine, lp.data(), lp.size()) != TRUE) {
        HTS_Engine_refresh(&s_engine);
        ESP_LOGE(kTag, "generate_state_sequence failed");
        return ESP_FAIL;
    }
    int64_t t_state_done = esp_timer_get_time();

    // 2.5. 文頭・文末 sil トリム: MLPG (パラメータ生成) より前に継続長を縮める。
    trim_lead_tail_sil(&s_engine, labels);

    // 3. パラメータ生成 (MLPG)
    size_t need = estimate_pstream_bytes(&s_engine);
    size_t pcm_capacity = s_engine.sss.total_frame * HTS_Engine_get_fperiod(&s_engine);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram < need + kPstreamReserve) {
        ESP_LOGW(kTag, "PSRAM preflight failed: need=%u free=%u",
                 (unsigned)need, (unsigned)free_psram);
        HTS_Engine_refresh(&s_engine);
        return ESP_ERR_NO_MEM;
    }

    if (HTS_Engine_generate_parameter_sequence(&s_engine) != TRUE) {
        HTS_Engine_refresh(&s_engine);
        ESP_LOGE(kTag, "generate_parameter_sequence failed");
        return ESP_FAIL;
    }
    int64_t t_mlpg_done = esp_timer_get_time();

    // 4. stream0 (mgc) & stream2 (lpf) float MLPG
    HTS_PStreamSet *pss = &s_engine.pss;
    size_t mgc_len = HTS_PStreamSet_get_vector_length(pss, 0);
    size_t lpf_len = HTS_PStreamSet_get_nstream(pss) >= 3 ? HTS_PStreamSet_get_vector_length(pss, 2) : 0;
    // DLS-033 (診断ログ): stream 0/1/2 それぞれの load / mlpg / par 書き戻しコピーの内訳を計測する。
    // 処理の順序・内容は変えず、既存の呼び出しの前後に esp_timer_get_time() を挟むだけ。
    int64_t t_s0_load_ms = 0, t_s0_mlpg_ms = 0;
    int64_t t_s1_load_ms = 0, t_s1_mlpg_ms = 0, t_s1_copy_ms = 0;
    int64_t t_s2_load_ms = 0, t_s2_mlpg_ms = 0, t_s2_copy_ms = 0;

    hts_tmpl::MlpgT<float> mlpg0;
    try {
        int64_t s0_t0 = esp_timer_get_time();
        mlpg0.load_from_sss(&pss->pstream[0], &s_engine.sss, 0);
        int64_t s0_t1 = esp_timer_get_time();
        mlpg0.mlpg();
        int64_t s0_t2 = esp_timer_get_time();
        mlpg0.release_after_mlpg();
        t_s0_load_ms = (s0_t1 - s0_t0) / 1000;
        t_s0_mlpg_ms = (s0_t2 - s0_t1) / 1000;
    } catch (const std::bad_alloc &) {
        HTS_Engine_refresh(&s_engine);
        ESP_LOGE(kTag, "mlpg0 bad_alloc");
        return ESP_ERR_NO_MEM;
    }
    // stream 1 (lf0 MSD) float MLPG (DLS-025)
    size_t lf0_len = HTS_PStreamSet_get_nstream(pss) >= 2 ? HTS_PStreamSet_get_vector_length(pss, 1) : 0;
    if (lf0_len > 0 && pss->pstream[1].length > 0) {
        try {
            hts_tmpl::MlpgT<float> mlpg1;
            int64_t s1_t0 = esp_timer_get_time();
            mlpg1.load(&pss->pstream[1]);
            int64_t s1_t1 = esp_timer_get_time();
            mlpg1.mlpg();
            int64_t s1_t2 = esp_timer_get_time();
            for (size_t t = 0; t < mlpg1.length; ++t) {
                for (size_t k = 0; k < lf0_len; ++k) {
                    pss->pstream[1].par[t][k] = static_cast<double>(mlpg1.par[t][k]);
                }
            }
            int64_t s1_t3 = esp_timer_get_time();
            t_s1_load_ms = (s1_t1 - s1_t0) / 1000;
            t_s1_mlpg_ms = (s1_t2 - s1_t1) / 1000;
            t_s1_copy_ms = (s1_t3 - s1_t2) / 1000;
        } catch (const std::bad_alloc &) {
            HTS_Engine_refresh(&s_engine);
            ESP_LOGE(kTag, "mlpg1 bad_alloc");
            return ESP_ERR_NO_MEM;
        }
    }
    // stream 2 (lpf) float MLPG
    if (lpf_len > 0) {
        try {
            hts_tmpl::MlpgT<float> mlpg2;
            int64_t s2_t0 = esp_timer_get_time();
            mlpg2.load_from_sss(&pss->pstream[2], &s_engine.sss, 2);
            int64_t s2_t1 = esp_timer_get_time();
            mlpg2.mlpg();
            int64_t s2_t2 = esp_timer_get_time();
            for (size_t t = 0; t < mlpg2.length; ++t) {
                for (size_t k = 0; k < lpf_len; ++k) {
                    pss->pstream[2].par[t][k] = static_cast<double>(mlpg2.par[t][k]);
                }
            }
            int64_t s2_t3 = esp_timer_get_time();
            t_s2_load_ms = (s2_t1 - s2_t0) / 1000;
            t_s2_mlpg_ms = (s2_t2 - s2_t1) / 1000;
            t_s2_copy_ms = (s2_t3 - s2_t2) / 1000;
        } catch (const std::bad_alloc &) {
            HTS_Engine_refresh(&s_engine);
            ESP_LOGE(kTag, "mlpg2 bad_alloc");
            return ESP_ERR_NO_MEM;
        }
    }
    int64_t t_mlpg0_done = esp_timer_get_time();
    ESP_LOGI(kTag, "[TIMING mlpg0-detail] s0 load=%lldms mlpg=%lldms | s1 load=%lldms mlpg=%lldms copy=%lldms | "
             "s2 load=%lldms mlpg=%lldms copy=%lldms",
             (long long)t_s0_load_ms, (long long)t_s0_mlpg_ms,
             (long long)t_s1_load_ms, (long long)t_s1_mlpg_ms, (long long)t_s1_copy_ms,
             (long long)t_s2_load_ms, (long long)t_s2_mlpg_ms, (long long)t_s2_copy_ms);

    // 5. Vocoder 合成 & チャンクストリーミング
    hts_tmpl::VocoderT<float, float> vocoder;
    vocoder.initialize(mgc_len - 1, HTS_Engine_get_sampling_frequency(&s_engine),
                       HTS_Engine_get_fperiod(&s_engine), 5, true);
    std::vector<float> spectrum(mgc_len);
    std::vector<float> lpf(lpf_len ? lpf_len : 1);
    std::vector<float> frame;
    frame.reserve(HTS_Engine_get_fperiod(&s_engine));

    constexpr size_t kChunkSize = 512;
    int16_t chunk_buf[kChunkSize];
    size_t chunk_count = 0;
    size_t total_pcm_emitted = 0;
    esp_err_t synth_error = ESP_OK;

    const auto emit = [&](size_t frame_index, const double *mgc, size_t mlen, double lf0, const double *lpf_in, size_t nlpf) {
        if (synth_error != ESP_OK) return;
        for (size_t i = 0; i < mlen; ++i) spectrum[i] = static_cast<float>(mgc[i]);
        for (size_t i = 0; i < nlpf; ++i) lpf[i] = static_cast<float>(lpf_in[i]);
        frame.clear();
        vocoder.synthesize_frame(mlen - 1, lf0, spectrum.data(), lpf.data(), nlpf,
                                 static_cast<float>(s_engine.condition.alpha),
                                 static_cast<float>(s_engine.condition.volume), frame);
        for (float sample : frame) {
            chunk_buf[chunk_count++] = clamp_pcm(sample);
            total_pcm_emitted++;
            if (chunk_count == kChunkSize) {
                if (!on_samples(chunk_buf, chunk_count, pcm_capacity, user_ctx)) {
                    synth_error = ESP_ERR_INVALID_STATE; // Aborted by caller
                    return;
                }
                chunk_count = 0;
            }
        }
        if (frame_index != 0 && (frame_index % 128) == 0) vTaskDelay(1);
    };

    hts_extract::extract_from_pss(&s_engine, emit,
        [&](size_t frame_index, size_t coefficient) { return mlpg0.par[frame_index][coefficient]; });

    // 残余サンプルを出力
    if (synth_error == ESP_OK && chunk_count > 0) {
        if (!on_samples(chunk_buf, chunk_count, pcm_capacity, user_ctx)) {
            synth_error = ESP_ERR_INVALID_STATE;
        }
    }

    // HTS_Engine_refresh() は sss (total_frame 含む) をクリアするため、ログ用に
    // refresh 前の値を退避しておく ([TIMING stream] が常に frames=0 と出る不具合の修正)。
    const size_t total_frame_for_log = s_engine.sss.total_frame;
    HTS_Engine_refresh(&s_engine);

    int64_t t_voc_done = esp_timer_get_time();
    int64_t dur_ms = (t_voc_done - t_start) / 1000;
    int64_t audio_ms = static_cast<int64_t>(total_pcm_emitted) * 1000 / 16000;
    float rtf = audio_ms > 0 ? static_cast<float>(dur_ms) / static_cast<float>(audio_ms) : 0.0f;

    int64_t front_ms = (t_front_done - t_start) / 1000;
    int64_t state_ms = (t_state_done - t_front_done) / 1000;
    int64_t mlpg_ms  = (t_mlpg_done - t_state_done) / 1000;
    int64_t mlpg0_ms = (t_mlpg0_done - t_mlpg_done) / 1000;
    int64_t voc_ms   = (t_voc_done - t_mlpg0_done) / 1000;

    ESP_LOGI(kTag, "[TIMING stream] '%s' (%u labels, %u frames, %u samples / %lldms audio): TOTAL=%lldms (RTF=%.2f)",
             text, (unsigned)labels.size(), (unsigned)total_frame_for_log, (unsigned)total_pcm_emitted, (long long)audio_ms, (long long)dur_ms, rtf);
    ESP_LOGI(kTag, "[TIMING breakdown] front=%lldms, state=%lldms, mlpg=%lldms, mlpg0=%lldms, vocoder=%lldms",
             (long long)front_ms, (long long)state_ms, (long long)mlpg_ms, (long long)mlpg0_ms, (long long)voc_ms);

    return synth_error;
}

// 従来のバッチ合成（全量 PSRAM バッファ確保して返す）
esp_err_t voice_hts_synthesize_text(const char *text, int16_t **out_pcm, size_t *out_sample_count) {
    ESP_RETURN_ON_FALSE(out_pcm != nullptr && out_sample_count != nullptr, ESP_ERR_INVALID_ARG, kTag, "null outputs");
    *out_pcm = nullptr;
    *out_sample_count = 0;

    struct BatchCtx {
        int16_t *buf = nullptr;
        size_t allocated = 0;
        size_t written = 0;
    } ctx;

    esp_err_t err = voice_hts_synthesize_text_stream(text, [](const int16_t *samples, size_t count, size_t total_est, void *user_ctx) -> bool {
        auto *c = static_cast<BatchCtx *>(user_ctx);
        if (c->buf == nullptr) {
            c->allocated = total_est;
            c->buf = static_cast<int16_t *>(heap_caps_malloc(total_est * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!c->buf) return false;
        }
        if (c->written + count > c->allocated) {
            size_t new_size = c->written + count + 1024;
            int16_t *new_buf = static_cast<int16_t *>(heap_caps_realloc(c->buf, new_size * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!new_buf) return false;
            c->buf = new_buf;
            c->allocated = new_size;
        }
        std::memcpy(c->buf + c->written, samples, count * sizeof(int16_t));
        c->written += count;
        return true;
    }, &ctx);

    if (err != ESP_OK) {
        if (ctx.buf) heap_caps_free(ctx.buf);
        return err;
    }

    *out_pcm = ctx.buf;
    *out_sample_count = ctx.written;
    return ESP_OK;
}
} // extern "C"
