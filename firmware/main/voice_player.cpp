#include "nk_event_core.h"
// DLS-019: ギャップレス連続ストリーミング再生 (notif DLS-096 準拠)
// DLS-020: 日本語TTS（HTS / OpenJTalk）移植とM5.Speaker連携
//
// 動作方針:
//   1. voice_hts_synthesize_text() でテキストから 16kHz PCM (PSRAM) を合成
//   2. 合成された PCM を 512 サンプルずつのチャンクに分割し、M5.Speaker (channel 1) の
//      2スロットキューイング (isPlaying < 2) を使ってギャップレス連続再生
//   3. HTS 未初期化または合成失敗時は、従来の 150 音 WAV 連結再生にフォールバック
//   4. 終端ゼロフラッシュによる DMA 残留音抑止

#include "voice_player.h"
#include "serial_say_core.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <M5Unified.hpp>

#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sano_voice.h"
#include "boot_progress.h"
#include "voice_config.h"
#include "ima_adpcm_decoder.h"
#include "voice_char_map_fw.h"
#include "filler_table.h"
#include "imu_sampler.h"
#include "trivia_table.h"
#include "task_stack_sizes.h"

namespace {

constexpr char kTag[] = "nk-voice";
constexpr char kMountPath[] = "/littlefs";
constexpr size_t kHeaderSize = 60;
constexpr uint16_t kExpectedFormat = 0x11;
constexpr uint16_t kExpectedChannels = 1;
constexpr uint32_t kExpectedSampleRate = 16000;
constexpr uint32_t kSanoSampleRate = 22050; // sanoTTS-jp の出力レート（本文もフィラーも sano で合成するので全て 22,050 Hz）
constexpr uint32_t kFillerPauseMs = 200; // DLS-035: フィラーと本文の間
constexpr uint16_t kExpectedBits = 4;
constexpr uint16_t kExpectedBlockAlign = 1024;
constexpr uint16_t kExpectedSamplesPerBlock = 2041;
constexpr int kVoiceChannel = 1; // 誘導音(kCueChannel=0)と干渉しないチャンネル
constexpr uint32_t kEnvelopePeriodUs = 20000;
// The longest one-shot filler is 2.37 s including its pause; 256 slots
// retain 5.12 s while keeping the BLE-sensitive internal RAM footprint small.
constexpr size_t kEnvelopeSlots = 256;
struct EnvelopeSlot {
    std::atomic<uint32_t> tick{UINT32_MAX};
    std::atomic<uint16_t> level{0};
};
static EnvelopeSlot s_envelope[kEnvelopeSlots];
static int64_t s_audio_next_us = 0; // voice task only

static void publish_envelope(const int16_t *samples, size_t count, int64_t start_us, uint32_t sample_rate) {
    // 10 ms windows keep the work bounded even for a whole filler WAV.
    for (size_t offset = 0; offset < count; offset += 160) {
        const size_t n = std::min<size_t>(160, count - offset);
        uint32_t sum = 0;
        for (size_t i = 0; i < n; ++i) {
            const int32_t value = samples[offset + i];
            sum += static_cast<uint32_t>(value < 0 ? -value : value);
        }
        const uint32_t tick = static_cast<uint32_t>((start_us + static_cast<int64_t>(offset) * 1000000 / sample_rate) / kEnvelopePeriodUs);
        auto &slot = s_envelope[tick % kEnvelopeSlots];
        const uint16_t level = static_cast<uint16_t>(sum / n);
        const uint16_t prior = slot.tick.load(std::memory_order_relaxed) == tick
            ? slot.level.load(std::memory_order_relaxed) : 0;
        slot.level.store(std::max(level, prior), std::memory_order_relaxed);
        slot.tick.store(tick, std::memory_order_release);
    }
}

// キューに積んだ音が鳴り終わる予想時刻（22,050 Hz 換算。playRaw はブロックしても先頭は max(終わり, 呼んだ時刻)）
static int64_t s_sano_play_end_us = 0;
// sample_rate は sano の合成音なら 22,050 Hz（既定）、LittleFS の WAV（play_file）は 16,000 Hz
static bool play_voice_pcm(const int16_t *samples, size_t count, uint32_t sample_rate = kSanoSampleRate) {
    const int64_t now_us = esp_timer_get_time();
    const bool active = M5.Speaker.isPlaying(kVoiceChannel) != 0;
    const int64_t start_us = active ? std::max(now_us, s_audio_next_us) : now_us;
    {
        const int64_t t_call = esp_timer_get_time();
        s_sano_play_end_us = std::max(s_sano_play_end_us, t_call) + static_cast<int64_t>(count) * 1000000 / sample_rate;
    }
    const bool accepted = M5.Speaker.playRaw(samples, count, sample_rate,
                                               false, 1, kVoiceChannel, false);
    if (accepted) {
        publish_envelope(samples, count, start_us, sample_rate);
        s_audio_next_us = start_us + static_cast<int64_t>(count) * 1000000 / sample_rate;
    }
    return accepted;
}

// DLS-019 / DLS-020: 再生バッファ
constexpr size_t kChunkSamples = 512;
constexpr size_t kRingCount = 3;
static int16_t s_pcm_chunk_ring[kRingCount][kChunkSamples];
static size_t  s_chunk_ring_idx = 0;

uint8_t s_encoded[kExpectedBlockAlign];
int16_t s_pcm_ring[kRingCount][kExpectedSamplesPerBlock];
size_t  s_ring_idx = 0;

static bool s_fs_mounted = false;
static QueueHandle_t s_voice_queue = nullptr;
static std::atomic<bool> s_playing(false);
static std::atomic<bool> s_voice_init_done(false);
static SemaphoreHandle_t s_audio_mutex = nullptr;
static bool s_listen_owned = false; // protected by s_audio_mutex
// stt32 DLS-1082: 起動語の待受が s_listen_owned を持っている間だけ true（s_audio_mutex で保護）。待受の持ち主は発話を拒否せず
// キューに残し、待受側が listen_release で譲る。聞き取り（1 回タップ）の持ち主では false のまま（従来どおり拒否）。
static bool s_listen_standby = false;
static std::atomic<bool> s_listen_yield(false); // a speech/cue needs the speaker: the standby owner should release
static bool s_model_write_owned = false; // 同じ音声ロックで保護
static std::atomic<bool> s_worker_busy(false); // includes dequeue -> playback

class VoiceAudioGuard {
public:
    VoiceAudioGuard() { if (s_audio_mutex) xSemaphoreTake(s_audio_mutex, portMAX_DELAY); }
    ~VoiceAudioGuard() { if (s_audio_mutex) xSemaphoreGive(s_audio_mutex); }
};
static std::atomic<uint32_t> s_tracked(NK_SPEECH_IDLE);
static uint32_t s_next_speech_ticket = 0; // serialized queued producers
static void cancel_tracked() {
    auto expected = s_tracked.load();
    while ((expected & 3u) == NK_SPEECH_PENDING
           && !s_tracked.compare_exchange_weak(expected, (expected & ~3u) | NK_SPEECH_CANCELLED)) {}
}
static std::atomic<bool> s_abort_requested(false);

uint16_t read_u16(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

// 終端ゼロフラッシュ
static void flush_silence_and_wait(void) {
    while (M5.Speaker.isPlaying(kVoiceChannel) && !s_abort_requested.load()) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!s_abort_requested.load()) {
        static const int16_t silence[512] = {0};
        play_voice_pcm(silence, sizeof(silence) / sizeof(silence[0]));
        while (M5.Speaker.isPlaying(kVoiceChannel)) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

// DLS-023: HTS ストリーミング再生コンテキスト
struct HtsPlayStreamCtx {
    size_t prebuf_threshold = 0;
    size_t total_buffered = 0;
    bool playback_started = false;
    uint32_t t_request = 0;
    uint32_t t_first_play = 0;
    // DLS-031: フィラー再生終了予定時刻 (t_request 基準の相対 ms)。フィラー無しの経路では 0。
    uint32_t filler_end_ms = 0;
    std::vector<int16_t> prebuf;
    // DLS-033 (診断ログ): 本文 PCM の先頭無音・末尾無音を、本文全体を保持せずに計測する。
    // コールバックをまたいで通し番号を進め、最後に |x| > 500 を観測したグローバル位置だけ覚える。
    size_t onset_samples_seen = 0;
    bool onset_logged = false;
    int64_t last_loud_sample_index = -1;  // 未観測なら -1
};

// チャンクを M5.Speaker にキューイング（isPlaying < 2 で先行挿入）
static bool queue_pcm_to_speaker(const int16_t *samples, size_t count) {
    size_t offset = 0;
    while (offset < count && !s_abort_requested.load()) {
        const size_t n = std::min(kChunkSamples, count - offset);
        int16_t *dst = s_pcm_chunk_ring[s_chunk_ring_idx];
        std::memcpy(dst, samples + offset, n * sizeof(int16_t));

        while (M5.Speaker.isPlaying(kVoiceChannel) >= 2) {
            if (s_abort_requested.load()) return false;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        play_voice_pcm(dst, n);
        s_chunk_ring_idx = (s_chunk_ring_idx + 1) % kRingCount;
        offset += n;
    }
    return !s_abort_requested.load();
}

static bool hts_stream_consumer_cb(const int16_t *samples, size_t count,
                                   size_t total_estimated_samples, void *user_ctx) {
    auto *ctx = static_cast<HtsPlayStreamCtx *>(user_ctx);
    if (s_abort_requested.load()) return false;

    // DLS-033 (診断ログ): 先頭無音・末尾無音の計測。本文全体は保持せず、通し番号と
    // 最後に |x| > 500 だったグローバル位置だけを追跡する（音声データそのものは変更しない）。
    for (size_t i = 0; i < count; ++i) {
        int32_t av = samples[i] < 0 ? -static_cast<int32_t>(samples[i]) : static_cast<int32_t>(samples[i]);
        if (av > 500) {
            if (!ctx->onset_logged) {
                ctx->onset_logged = true;
                const size_t onset_idx = ctx->onset_samples_seen + i;
                const float onset_ms = (float)onset_idx * 1000.0f / (float)kExpectedSampleRate;
                ESP_LOGI(kTag, "[BODY ONSET] first |x|>500 at sample %u (%.1f ms from body start)",
                         (unsigned)onset_idx, onset_ms);
            }
            ctx->last_loud_sample_index = (int64_t)(ctx->onset_samples_seen + i);
        }
    }
    ctx->onset_samples_seen += count;

    // 初回チャンク時にプリバッファ閾値を計算
    if (ctx->prebuf_threshold == 0) {
        // DLS-025: プリバッファを 500ms (8000 samples = 16kHz * 0.5s) に短縮し発話開始を大幅高速化
        // DLS-026 (案A): Vocoder RTF 0.37 の優位性を活かし、プリバッファを 150ms (2400 samples) に短縮
        ctx->prebuf_threshold = std::min<size_t>(total_estimated_samples, 2400);
        ctx->prebuf.reserve(ctx->prebuf_threshold + count);
    }

    if (!ctx->playback_started) {
        // プリバッファ蓄積フェーズ
        ctx->prebuf.insert(ctx->prebuf.end(), samples, samples + count);
        ctx->total_buffered += count;
        if (ctx->total_buffered >= ctx->prebuf_threshold) {
            // プリバッファ到達 -> 再生開始！
            // プリバッファ全量を一度に queue_pcm_to_speaker（isPlaying < 2 でブロッキング）してしまうと
            // 再生消化待ちで Vocoder 生成タスクが停止するため、先頭のリングバッファ分（最大 2 チャンク）のみ
            // まず M5.Speaker に投入し、残りは prebuf に残して逐次生成チャンクと一緒に流す。
            ctx->playback_started = true;
            ctx->t_first_play = (uint32_t)(esp_timer_get_time() / 1000);
            uint32_t delay_ms = ctx->t_first_play - ctx->t_request;
            ESP_LOGI(kTag, "[STREAM] Prebuffer reached (%u samples / %.1f ms audio) -> Playback START (latency=%u ms)",
                     (unsigned)ctx->prebuf.size(), (float)ctx->prebuf.size() * 1000.0f / 16000.0f, (unsigned)delay_ms);
            // DLS-031: 本文プリバッファ到達時刻とフィラー再生終了予定時刻のギャップを計測。
            // 負値 = 本文の準備がフィラー終了より先に間に合っている（望ましい）。
            // 正値 = フィラーが先に鳴り終わり、本文が間に合わず無音が生じている。
            int32_t gap_ms = (int32_t)delay_ms - (int32_t)ctx->filler_end_ms;
            ESP_LOGI(kTag, "[PIPELINE GAP] body ready at %u ms, filler ends at %u ms -> gap=%d ms",
                     (unsigned)delay_ms, (unsigned)ctx->filler_end_ms, (int)gap_ms);
        }
    } else {
        ctx->prebuf.insert(ctx->prebuf.end(), samples, samples + count);
    }

    if (ctx->playback_started && !ctx->prebuf.empty()) {
        // M5.Speaker のキューに空きがあれば prebuf から順次流す（ブロッキングしない）
        while (M5.Speaker.isPlaying(kVoiceChannel) < 2 && !ctx->prebuf.empty()) {
            if (s_abort_requested.load()) return false;
            size_t n = std::min(kChunkSamples, ctx->prebuf.size());
            int16_t *dst = s_pcm_chunk_ring[s_chunk_ring_idx];
            std::memcpy(dst, ctx->prebuf.data(), n * sizeof(int16_t));
            play_voice_pcm(dst, n);
            s_chunk_ring_idx = (s_chunk_ring_idx + 1) % kRingCount;
            ctx->prebuf.erase(ctx->prebuf.begin(), ctx->prebuf.begin() + n);
        }
    }

    return !s_abort_requested.load();
}

// ---- sano 経路の再生の渡し方。sanoTTS の boards/m5unified/main/saan_audio_m5.cpp と同じ形 ----
// プリロール 4 チャンク（8,192 sample）を 1 回の playRaw、以後は pull 1 回分（2,048 sample）を
// 1 回の playRaw（リング 3 本。playRaw はキューが満杯ならブロックする）。isPlaying での先行制限はしない。
constexpr size_t kSanoPullSamples = 2048;
constexpr size_t kSanoPrerollSamples = 8192;
constexpr int kSanoRingN = 3;
static int16_t *s_sano_pre = nullptr;
static int16_t *s_sano_ring[kSanoRingN];
static size_t s_sano_ridx = 0;
static size_t s_sano_fill = 0;
static bool s_sano_in_preroll = true;

// シリアルの `say` の計測（DLS-159）。nk_voice タスクだけが触る。
//   アンダーラン = 2 回目以降の PCM を M5.Speaker に渡す時点で、スピーカーのキューが空（isPlaying == 0）だった回数
//   （= 再生が合成に追いついて音が途切れた）。区間の境目（sano_boundary_cb）の次の 1 回は、文の間の無音を入れる
//   設計なので数えない。末尾の flush は数えない。
//   鳴らし始め = 要求（voice_player_say_diag を呼んだ時刻）から、最初の PCM を M5.Speaker に渡した時刻まで。
static bool s_diag_on = false;
static bool s_diag_skip = false;
static int s_diag_underruns = 0;
static int64_t s_diag_first_us = 0;

static bool sano_stream_consumer_cb(const int16_t *samples, size_t count,
                                    size_t total_estimated_samples, void *user_ctx) {
    auto *ctx = static_cast<HtsPlayStreamCtx *>(user_ctx);
    if (s_abort_requested.load()) return false;
    if (s_sano_pre == nullptr) {
        s_sano_pre = static_cast<int16_t *>(heap_caps_malloc(kSanoPrerollSamples * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        for (int i = 0; i < kSanoRingN; ++i)
            s_sano_ring[i] = static_cast<int16_t *>(heap_caps_malloc(kSanoPullSamples * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_sano_pre || !s_sano_ring[0] || !s_sano_ring[1] || !s_sano_ring[2]) {
            ESP_LOGE(kTag, "sano 再生バッファの確保に失敗");
            // 次の発話で確保し直せるように、確保できた分も返して全部 nullptr に戻す
            heap_caps_free(s_sano_pre);
            s_sano_pre = nullptr;
            for (int i = 0; i < kSanoRingN; ++i) {
                heap_caps_free(s_sano_ring[i]);
                s_sano_ring[i] = nullptr;
            }
            return false;
        }
    }
    size_t seen = ctx->total_buffered;
    if (seen == 0) { s_sano_fill = 0; s_sano_in_preroll = true; }
    int16_t *dst = s_sano_in_preroll ? s_sano_pre : s_sano_ring[s_sano_ridx];
    std::memcpy(dst + s_sano_fill, samples, count * sizeof(int16_t));
    s_sano_fill += count;
    seen += count;
    ctx->total_buffered = seen;
    const size_t limit = s_sano_in_preroll ? kSanoPrerollSamples : kSanoPullSamples;
    if (s_sano_fill >= limit || seen >= total_estimated_samples) {
        if (s_diag_on) {
            if (!s_sano_in_preroll) {
                if (s_diag_skip) s_diag_skip = false;
                else if (M5.Speaker.isPlaying(kVoiceChannel) == 0) ++s_diag_underruns;
            }
            if (s_diag_first_us == 0) s_diag_first_us = esp_timer_get_time();
        }
        play_voice_pcm(dst, s_sano_fill);
        if (s_sano_in_preroll) s_sano_in_preroll = false;
        else s_sano_ridx = (s_sano_ridx + 1) % kSanoRingN;
        s_sano_fill = 0;
    }
    return !s_abort_requested.load();
}


// 半端にたまっている分を送る（合成の最後・区間の境目）
static void sano_consumer_flush() {
    if (s_sano_fill == 0) return;
    int16_t *src = s_sano_in_preroll ? s_sano_pre : s_sano_ring[s_sano_ridx];
    if (s_diag_on && s_diag_first_us == 0) s_diag_first_us = esp_timer_get_time();
    play_voice_pcm(src, s_sano_fill);
    if (s_sano_in_preroll) s_sano_in_preroll = false;
    else s_sano_ridx = (s_sano_ridx + 1) % kSanoRingN;
    s_sano_fill = 0;
}

// 2 つ目以降の区間: 最初の pull が済んだ後に呼ばれる。文の間を 0.5 s に揃える無音を入れる。
// 無音の総量 = 目標 − すでに空いた時間（スピーカーが空だった分）− 前の区間の末尾の無音 − 次の区間の先頭の無音
constexpr int64_t kSanoSentenceGapUs = 500000;
static int16_t *s_sano_zero = nullptr;
static void sano_boundary_cb(void *) {
    if (s_diag_on) s_diag_skip = true;
    sano_consumer_flush();
    const sano_boundary_info_t bi = sano_boundary_info();
    const int64_t now = esp_timer_get_time();
    const int64_t idle_us = now > s_sano_play_end_us ? now - s_sano_play_end_us : 0;
    const int64_t remain_us = s_sano_play_end_us > now ? s_sano_play_end_us - now : 0;
    const int64_t pcm_us = (static_cast<int64_t>(bi.tail_samples) + bi.lead_samples) * 1000000 / kSanoSampleRate;
    int64_t add_us = kSanoSentenceGapUs - idle_us - pcm_us;
    if (add_us < 0) add_us = 0;
    size_t add = static_cast<size_t>(add_us * kSanoSampleRate / 1000000);
    ESP_LOGI(kTag, "[SANO GAP] 前の音の残り %lld ms / すでに空いた %lld ms / PCM の末尾+先頭の無音 %lld ms (tail %d, lead %d sample) -> 無音を %u sample (%lld ms) 足す",
             (long long)(remain_us / 1000), (long long)(idle_us / 1000), (long long)(pcm_us / 1000),
             (int)bi.tail_samples, (int)bi.lead_samples, (unsigned)add, (long long)add_us / 1000);
    if (!s_sano_zero) {
        s_sano_zero = static_cast<int16_t *>(heap_caps_calloc(kSanoPullSamples, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_sano_zero) return;
    }
    while (add > 0 && !s_abort_requested.load()) {
        const size_t n = std::min(add, kSanoPullSamples);
        play_voice_pcm(s_sano_zero, n);
        add -= n;
    }
}

// ---- 起動時に sano でフィラーを合成して PSRAM にためる（22,050 Hz int16。末尾に kFillerPauseMs の無音を含む）----
struct SanoFiller { int16_t *pcm; size_t n; };
static SanoFiller s_sano_fillers[9];   // 0..7 = kFillerTable、8 = trivia_intro（kTriviaPrefix）
static const char kTriviaIntroPath[] = "/filler/trivia_intro.wav";
static const SanoFiller *sano_filler_find(const char *wav_path) {
    if (wav_path == nullptr) return nullptr;
    for (size_t i = 0; i < kFillerTableCount && i < 8; ++i)
        if (std::strcmp(kFillerTable[i].wav_path, wav_path) == 0) return s_sano_fillers[i].pcm ? &s_sano_fillers[i] : nullptr;
    if (std::strcmp(kTriviaIntroPath, wav_path) == 0) return s_sano_fillers[8].pcm ? &s_sano_fillers[8] : nullptr;
    return nullptr;
}
struct PreloadCtx { int16_t *buf; size_t n, cap; };
static bool sano_preload_cb(const int16_t *s, size_t cnt, size_t total, void *u) {
    auto *c = static_cast<PreloadCtx *>(u);
    if (total == static_cast<size_t>(-1)) return false;   // 分割された = 想定外
    const size_t pause = static_cast<size_t>(kSanoSampleRate) * kFillerPauseMs / 1000;
    if (!c->buf) {
        c->cap = total + pause;
        c->buf = static_cast<int16_t *>(heap_caps_calloc(c->cap, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!c->buf) return false;
    }
    if (c->n + cnt > c->cap) return false;
    std::memcpy(c->buf + c->n, s, cnt * sizeof(int16_t));
    c->n += cnt;
    return true;
}
static void show_boot_voice_name() {
    char label[40];
    const char *name = sano_voice_model_name();
    const char *sha8 = sano_voice_model_sha8();
    snprintf(label, sizeof label, "voice: %s%s", name ? "" : (sha8 ? "#" : ""),
             name ? name : (sha8 ? sha8 : "?"));
    nk_boot_voice_name(label);
}
static void sano_fillers_preload() {
    const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const int64_t t0 = esp_timer_get_time();
    size_t bytes = 0;
    // プログレスバー用: 各フィラーの合成時間の見込み（ms。実測の比）と累計
    static const int kExpectMs[9] = {630, 633, 579, 564, 563, 506, 506, 561, 1226};
    int cum_ms = 0;
    for (int i = 0; i < 9; ++i) {
        const char *text = i < 8 ? kFillerTable[i].text : kTriviaPrefix;
        PreloadCtx c{nullptr, 0, 0};
        const int64_t t1 = esp_timer_get_time();
        const esp_err_t err = sano_voice_synthesize_text_stream(text, sano_preload_cb, &c);
        const double ms = (double)(esp_timer_get_time() - t1) / 1000.0;
        cum_ms += kExpectMs[i];
        {
            char lb[24];
            snprintf(lb, sizeof lb, "FILLER %d/9", i + 1);
            nk_boot_progress(lb, 19 + 81 * cum_ms / 5768);
        }
        if (err == ESP_OK && c.buf) {
            s_sano_fillers[i] = {c.buf, c.cap};
            bytes += c.cap * sizeof(int16_t);
            ESP_LOGI(kTag, "[FILLER PRELOAD] %d '%s': 合成 %.1f ms / 音声 %.3f s + 間 %u ms = %u sample (%u B)", i, text, ms,
                     (double)c.n / kSanoSampleRate, (unsigned)kFillerPauseMs, (unsigned)c.cap, (unsigned)(c.cap * 2));
        } else {
            ESP_LOGE(kTag, "[FILLER PRELOAD] %d '%s' の合成に失敗 (%s)", i, text, esp_err_to_name(err));
            if (c.buf) heap_caps_free(c.buf);
        }
    }
    ESP_LOGI(kTag, "[FILLER PRELOAD] 9 本の合成の合計 %.1f ms / PSRAM 使用 %u B（free %u -> %u）/ 内部 DRAM free %u -> %u",
             (double)(esp_timer_get_time() - t0) / 1000.0, (unsigned)bytes, (unsigned)psram_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)int_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}
static esp_err_t resolve_asset_path(const char *asset_path, char *path, size_t size) {
    const int length = std::snprintf(path, size, "%s%s", kMountPath, asset_path);
    return length < 0 || static_cast<size_t>(length) >= size ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t play_file(const char *asset_path) {
    char path[160];
    esp_err_t path_err = resolve_asset_path(asset_path, path, sizeof(path));
    if (path_err != ESP_OK) return path_err;

    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGE(kTag, "open failed: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t header[kHeaderSize];
    esp_err_t status = ESP_FAIL;
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header)) {
        ESP_LOGE(kTag, "short WAV header: %s", path);
        goto done;
    }
    if (std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0 ||
        std::memcmp(header + 12, "fmt ", 4) != 0 || std::memcmp(header + 52, "data", 4) != 0 ||
        read_u16(header + 20) != kExpectedFormat ||
        read_u16(header + 22) != kExpectedChannels ||
        read_u32(header + 24) != kExpectedSampleRate ||
        read_u16(header + 32) != kExpectedBlockAlign ||
        read_u16(header + 34) != kExpectedBits ||
        read_u16(header + 38) != kExpectedSamplesPerBlock) {
        ESP_LOGE(kTag, "unsupported WAV format: %s", path);
        status = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }

    {
        const uint32_t data_size = read_u32(header + 56);
        if (data_size == 0 || data_size % kExpectedBlockAlign != 0) {
            ESP_LOGE(kTag, "invalid WAV data size=%lu: %s", (unsigned long)data_size, path);
            status = ESP_ERR_INVALID_SIZE;
            goto done;
        }
        for (uint32_t offset = 0; offset < data_size; offset += kExpectedBlockAlign) {
            if (s_abort_requested.load()) {
                status = ESP_ERR_INVALID_STATE;
                goto done;
            }
            if (std::fread(s_encoded, 1, sizeof(s_encoded), file) != sizeof(s_encoded)) {
                ESP_LOGE(kTag, "short ADPCM block: %s", path);
                status = ESP_ERR_INVALID_SIZE;
                goto done;
            }

            int16_t *dst = s_pcm_ring[s_ring_idx];
            const int samples = notif_ima_adpcm_decode_block(
                dst, s_encoded, sizeof(s_encoded), kExpectedChannels);
            if (samples != kExpectedSamplesPerBlock) {
                ESP_LOGE(kTag, "ADPCM decode failed: %s", path);
                status = ESP_ERR_INVALID_RESPONSE;
                goto done;
            }

            while (M5.Speaker.isPlaying(kVoiceChannel) >= 2) {
                if (s_abort_requested.load()) {
                    status = ESP_ERR_INVALID_STATE;
                    goto done;
                }
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            play_voice_pcm(dst, samples, kExpectedSampleRate);
            s_ring_idx = (s_ring_idx + 1) % kRingCount;
        }
    }
    status = ESP_OK;

done:
    std::fclose(file);
    return status;
}

static size_t utf8_codepoint_len(uint8_t b) {
    if (b < 0x80) return 1;
    if (b < 0xC0) return 1;
    if (b < 0xE0) return 2;
    if (b < 0xF0) return 3;
    return 4;
}

static void katakana_to_hiragana(char *utf8) {
    uint8_t *p = reinterpret_cast<uint8_t *>(utf8);
    while (*p) {
        if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            uint16_t cp = ((uint16_t)(p[0] & 0x0F) << 12) |
                          ((uint16_t)(p[1] & 0x3F) << 6) |
                          (uint16_t)(p[2] & 0x3F);
            if (cp >= 0x30A1 && cp <= 0x30F6) {
                cp -= 0x60;
                p[0] = 0xE0 | ((cp >> 12) & 0x0F);
                p[1] = 0x80 | ((cp >> 6) & 0x3F);
                p[2] = 0x80 | (cp & 0x3F);
            }
            p += 3;
        } else {
            const size_t n = utf8_codepoint_len(*p);
            for (size_t i = 1; i < n; ++i) {
                if (!p[i]) return;
            }
            p += n;
        }
    }
}

static void normalize_greeting(char *utf8) {
    static const char kKonnichiha[] = "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF";
    char *found = std::strstr(utf8, kKonnichiha);
    while (found) {
        found[12] = '\xE3';
        found[13] = '\x82';
        found[14] = '\x8F';
        found = std::strstr(found + 15, kKonnichiha);
    }
}

// フォールバック用の WAV 連結再生
static void play_phrase_wav_fallback(const char *text) {
    char work[128];
    std::strncpy(work, text, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';
    katakana_to_hiragana(work);
    normalize_greeting(work);

    size_t position = 0;
    size_t played = 0;
    while (position < sizeof(work) && work[position] != '\0' && !s_abort_requested.load()) {
        if (work[position] == ' ' || work[position] == '\t') {
            position++;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (std::strncmp(work + position, "、", 3) == 0 || std::strncmp(work + position, "，", 3) == 0) {
            position += 3;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (std::strncmp(work + position, "。", 3) == 0 || std::strncmp(work + position, "．", 3) == 0) {
            position += 3;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        size_t best_index = SIZE_MAX;
        size_t best_length = 0;
        for (size_t index = 0; index < VOICE_WAV_COUNT; ++index) {
            const size_t key_length = std::strlen(VOICE_CHAR_KEYS[index]);
            if (key_length > best_length &&
                std::strncmp(work + position, VOICE_CHAR_KEYS[index], key_length) == 0) {
                best_index = index;
                best_length = key_length;
            }
        }
        if (best_index == SIZE_MAX) {
            ESP_LOGW(kTag, "char map miss at byte %u: 0x%02X", (unsigned)position, (uint8_t)work[position]);
            const size_t n = utf8_codepoint_len((uint8_t)work[position]);
            size_t i = 1;
            while (i < n && position + i < sizeof(work) && work[position + i]) ++i;
            if (i < n) break;
            position += n;
            continue;
        }

        esp_err_t err = play_file(VOICE_WAV_FILES[best_index]);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "play_file failed: %s", VOICE_WAV_FILES[best_index]);
        }
        position += best_length;
        ++played;
    }

    flush_silence_and_wait();
    ESP_LOGI(kTag, "played WAV fallback '%s' (%u tokens)", text, (unsigned)played);
}

// P7-3: 相づち・フィラー辞書検索
const char *nk_find_filler_wav(const char *text) {
    if (text == nullptr || *text == '\0') return nullptr;
    // 1. 完全一致
    for (size_t i = 0; i < kFillerTableCount; ++i) {
        if (std::strcmp(text, kFillerTable[i].text) == 0) {
            return kFillerTable[i].wav_path;
        }
    }
    // 2. 句読点（、。）等を除去した完全一致
    char clean_text[64];
    size_t d = 0;
    for (size_t s = 0; text[s] != '\0' && d < sizeof(clean_text) - 1; ++s) {
        if ((uint8_t)text[s] == 0xE3 && (uint8_t)text[s+1] == 0x80 && 
            ((uint8_t)text[s+2] == 0x81 || (uint8_t)text[s+2] == 0x82)) { // 「、」または「。」
            s += 2;
            continue;
        }
        if (text[s] == ',' || text[s] == '.') continue;
        clean_text[d++] = text[s];
    }
    clean_text[d] = '\0';
    if (d > 0 && std::strcmp(clean_text, text) != 0) {
        for (size_t i = 0; i < kFillerTableCount; ++i) {
            if (std::strcmp(clean_text, kFillerTable[i].text) == 0) {
                return kFillerTable[i].wav_path;
            }
        }
    }
    return nullptr;
}

// `say` の 1 発話ぶんのログを 1 行出す（合成が終わった直後。再生の終わりは待たない）。
static void log_say_report(const char *text, bool completed, int64_t t_req_us) {
    const sano_utt_stats_t st = sano_voice_last_stats();
    static const char *const kRoute[] = {"かな", "辞書", "拒否"};
    nk::SayReport r;
    r.text = text;
    r.route = (st.route >= 0 && st.route <= 2) ? kRoute[st.route] : "不明";
    r.g2p_status = st.g2p_status ? st.g2p_status_text : "OK";
    r.ids = st.ids; r.segments = st.segments;
    r.g2p_ms = st.g2p_us / 1000.0; r.init_ms = st.init_us / 1000.0; r.pull_ms = st.pull_us / 1000.0;
    r.audio_s = (double)st.samples / kSanoSampleRate;
    r.pulls = st.pulls; r.full_pulls = st.full_pulls; r.over_budget = st.over_budget;
    r.full_med_ms = st.full_med_us / 1000.0; r.full_mean_ms = st.full_mean_us / 1000.0; r.chunk_ms = st.chunk_us / 1000.0;
    r.underruns = s_diag_underruns;
    r.start_ms = s_diag_first_us ? (double)(s_diag_first_us - t_req_us) / 1000.0 : -1;
    r.completed = completed;
    char line[640];
    nk::format_say_log(line, sizeof line, r);
    ESP_LOGI(kTag, "%s", line);
    if (st.pull_logged >= 256) ESP_LOGW(kTag, "[SAY] pull が 256 回を超えたため、定常 xRT は最初の 256 回の満チャンクだけの値");
}

bool play_phrase(const char *text, bool exact = false, bool diag = false, bool verbose = false, int64_t t_req_us = 0) {
    const int64_t t_play_phrase_us = esp_timer_get_time();
    if (text == nullptr || *text == '\0') return false;
    struct SpeechEventDone {
        bool active = false;
        void start(const char *body) {
            if (!active) { active = true; nk::Event("speech").text("phase", "start").text("text", body).emit(); }
        }
        ~SpeechEventDone() { if (active) nk::Event("speech").text("phase", "done").emit(); }
    } speech_event;
    const bool trivia = std::strncmp(text, kTriviaPrefix, sizeof(kTriviaPrefix) - 1) == 0;
    s_playing.store(true);
    s_abort_requested.store(false);

    // DLS-024: 発話中のセンサーサスペンド（音声全集中・Core 1 開放）
    nk_imu_suspend();

    // P7-3: 単体相づち・フィラーキャッシュ（4-bit IMA-ADPCM）に完全一致するか判定
    // diag（シリアルの say）は合成の速さを測るので、フィラーの完全一致で合成を飛ばさない
    const char *filler_wav = diag ? nullptr : nk_find_filler_wav(text);
    if (filler_wav != nullptr) {
        speech_event.start(text);
        ESP_LOGI(kTag, "[FILLER EXACT HIT] '%s' -> %s (Zero-latency direct playback)", text, filler_wav);
        // sano: 起動時に合成して PSRAM にためた PCM を鳴らす（WAV は使わない）
        const SanoFiller *hit = sano_filler_find(filler_wav);
        esp_err_t err = ESP_ERR_NOT_FOUND;
        if (hit != nullptr) {
            const int64_t t_hit = esp_timer_get_time();
            err = play_voice_pcm(hit->pcm, hit->n) ? ESP_OK : ESP_FAIL;
            ESP_LOGI(kTag, "[FILLER] 完全一致の再生: 要求から playRaw まで %lld us / 長さ %u sample (%.0f ms, 間を含む)",
                     (long long)(t_hit - t_play_phrase_us), (unsigned)hit->n, hit->n * 1000.0 / kSanoSampleRate);
        }
        if (err == ESP_OK) {
            flush_silence_and_wait();
            nk_imu_resume();
            s_playing.store(false);
            return !s_abort_requested.load();
        }
        ESP_LOGW(kTag, "play_file failed for filler '%s': %s, falling back", filler_wav, esp_err_to_name(err));
    }

    // P7-6 / DLS-031: 並行合成パイプライン (フィラー先行再生 + HTS バックグラウンド並行合成)
    // DLS-031: フィラーを 512 サンプルのチャンクへ分割して 2 スロットキューイングに乗せると、
    // front/state/MLPG 実行中は誰もチャンクを補充できずぶつ切れになる（棄却済み仮説）。
    // 代わりにフィラー(+無音)を 1 回の playRaw でまとめて投入し、M5.Speaker のスロット1つを
    // 占有させたまま最後まで途切れず鳴らし切る。本文は stream_ctx.prebuf（スロット2）側で
    // 既存ロジックのままプリバッファ・逐次キューイングする。
    if (!exact && sano_voice_is_ready() && kFillerTableCount > 0) {
        uint32_t t_req = (uint32_t)(esp_timer_get_time() / 1000);
        const nk_filler_entry_t trivia_intro = {kTriviaPrefix, "/filler/trivia_intro.wav"};
        const nk_filler_entry_t *f_entry = trivia ? &trivia_intro
            : &kFillerTable[esp_random() % kFillerTableCount];

        // フィラー PCM は M5.Speaker が実際に読み終える（または stop() で参照を断つ）まで
        // 生存させる必要がある。M5.Speaker.playRaw() はデータをコピーせず、再生タスクが
        // 後からこのポインタを直接読むため、このブロックのスコープを抜けて filler_pcm が
        // 破棄される前に、必ずどちらかの状態を保証する（正常終了/abort/合成失敗の全出口）。
        // sano のフィラー PCM は起動時に PSRAM へ合成済み（永続。playRaw が後から読むので解放しない）
        bool filler_queued = false;
        uint32_t filler_end_ms = 0;
        int64_t filler_dur_us = 0;

        const SanoFiller *sf = sano_filler_find(f_entry->wav_path);
        if (sf != nullptr) {
            const float filler_total_ms = (float)sf->n * 1000.0f / (float)kSanoSampleRate;
            filler_dur_us = static_cast<int64_t>(sf->n) * 1000000 / kSanoSampleRate;

            // 分割せず 1 回の playRaw でまとめて投入（ぶつ切れ防止の本体）。
            filler_queued = play_voice_pcm(sf->pcm, sf->n);
            // Remove the prefix only after its audio was successfully queued.
            if (trivia && filler_queued) text += sizeof(kTriviaPrefix) - 1;

            const uint32_t t_first = (uint32_t)(esp_timer_get_time() / 1000);
            filler_end_ms = filler_queued ? (t_first - t_req) + (uint32_t)(filler_dur_us / 1000) : 0;

            ESP_LOGI(kTag, "[PIPELINE START] Lead filler '%s' (total=%.0f ms incl. %ums pause, single playRaw, "
                     "22050 Hz) -> queued=%u (latency=%u ms)",
                     f_entry->text, filler_total_ms, (unsigned)kFillerPauseMs,
                     (unsigned)filler_queued, (unsigned)(t_first - t_req));
        } else {
            ESP_LOGW(kTag, "sano filler が無い (%s)、フィラー無しで続ける", f_entry->wav_path);
        }

        // 本文用のストリームコンテキストは新規に作る（playback_started=false からの
        // 既存プリバッファロジックそのまま）。filler_end_ms のみギャップ計測用に渡す。
        HtsPlayStreamCtx stream_ctx;
        stream_ctx.t_request = t_req;
        stream_ctx.filler_end_ms = filler_end_ms;

        // フィラーが再生されている「裏」で、直ちに本文の HTS 音声合成を開始！
        speech_event.start(text);
        ESP_LOGI(kTag, "[PIPELINE SYNTH] Starting concurrent HTS synth for body: '%s'...", text);
        esp_err_t err = sano_voice_synthesize_text_stream(text, sano_stream_consumer_cb, &stream_ctx);
        sano_consumer_flush();
        uint32_t t_synth_done = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGI(kTag, "[PIPELINE SYNTH DONE] Body synth finished in %u ms (total from request: %u ms)",
                 (unsigned)(t_synth_done - stream_ctx.t_request), (unsigned)(t_synth_done - t_req));
        // DLS-033 (診断ログ): 末尾無音 = 本文の総サンプル数から最後に |x|>500 だった位置を引いた
        // 残り。本文全体は保持していないので、コールバックが記録した通し番号だけで計算する。
        if (stream_ctx.onset_samples_seen > 0) {
            const int64_t tail_samples = stream_ctx.last_loud_sample_index >= 0
                ? (int64_t)stream_ctx.onset_samples_seen - stream_ctx.last_loud_sample_index - 1
                : (int64_t)stream_ctx.onset_samples_seen;
            const float tail_ms = (float)tail_samples * 1000.0f / (float)kExpectedSampleRate;
            ESP_LOGI(kTag, "[BODY TAIL SILENCE] %lld samples (%.1f ms) after last |x|>500 (body total %u samples)",
                     (long long)tail_samples, tail_ms, (unsigned)stream_ctx.onset_samples_seen);
        }

        if (err == ESP_OK) {
            // 合成完了後、キューに残っている全サンプルを M5.Speaker に流し切る
            if (!stream_ctx.prebuf.empty() && !s_abort_requested.load()) {
                queue_pcm_to_speaker(stream_ctx.prebuf.data(), stream_ctx.prebuf.size());
                stream_ctx.prebuf.clear();
            }
            // flush_silence_and_wait() は isPlaying(kVoiceChannel) が 0 になるまで待つ
            // （abort 時は待たずに戻るが、その場合は abort 元が既に stop() 済み）。
            // ここまで来ればフィラー分も含めて再生が完全に終わっているか停止済みなので、
            // この直後に filler_pcm がスコープを抜けて破棄されても安全。
            flush_silence_and_wait();
            nk_imu_resume();
            s_playing.store(false);
            return !s_abort_requested.load();
        }
        ESP_LOGW(kTag, "Concurrent HTS stream synth failed (%s), falling back", esp_err_to_name(err));
        // 合成失敗（abort 含む）でこのブロックを抜ける経路: フィラーがまだ再生中の可能性が
        // あるため、filler_pcm を破棄する前に明示的に stop してポインタ参照を断つ。
        // abort 経路は voice_player_say()/voice_player_stop() が既に
        // M5.Speaker.stop(kVoiceChannel) 済みだが、非 abort の合成失敗ではまだ止めていないので
        // ここで必ず止める（同チャンネルへの stop() の重複呼び出しは無害）。
        if (filler_queued) {
            M5.Speaker.stop(kVoiceChannel);
        }
        if (s_abort_requested.load()) {
            nk_imu_resume();
            s_playing.store(false);
            return !s_abort_requested.load();
        }
        // filler_pcm はこの直後にブロックスコープを抜けて破棄されるが、上記 stop() により安全。
    }

    // sano 経路: exact でも文を区切らず全体を 1 回で合成する（sano_voice が上限を超えるときだけ。で分割し、
    // 区間の間は 0.5 s の無音を入れる）。
    if (exact) {
        bool completed = sano_voice_is_ready();
        if (completed) {
            HtsPlayStreamCtx stream_ctx;
            stream_ctx.t_request = (uint32_t)(esp_timer_get_time() / 1000);
            speech_event.start(text);
            ESP_LOGI(kTag, "[EXACT SYNTH] '%s'", text);
            if (diag) {
                s_diag_on = true; s_diag_skip = false; s_diag_underruns = 0; s_diag_first_us = 0;
                sano_voice_set_verbose(verbose);
            }
            const esp_err_t err = sano_voice_synthesize_text_stream(text, sano_stream_consumer_cb, &stream_ctx);
            sano_consumer_flush();
            completed = err == ESP_OK && !s_abort_requested.load();
            if (diag) {
                log_say_report(text, completed, t_req_us);
                s_diag_on = false;
                sano_voice_set_verbose(false);
            }
            if (completed) flush_silence_and_wait();
            else ESP_LOGW(kTag, "[EXACT SYNTH FAILED] %s", esp_err_to_name(err));
        }
        completed = completed && !s_abort_requested.load();
        if (!completed) M5.Speaker.stop(kVoiceChannel);
        ESP_LOGI(kTag, "[EXACT FINISHED] completed=%d", (int)completed);
        nk_imu_resume();
        s_playing.store(false);
        return completed;
    }

    // 2. HTS が初期化されていない場合のフォールバック (従来動作)
    speech_event.start(text);
    if (sano_voice_is_ready()) {
        HtsPlayStreamCtx stream_ctx;
        stream_ctx.t_request = (uint32_t)(esp_timer_get_time() / 1000);
        esp_err_t err = sano_voice_synthesize_text_stream(text, sano_stream_consumer_cb, &stream_ctx);
        sano_consumer_flush();
        if (err == ESP_OK) {
            if (!stream_ctx.prebuf.empty() && !s_abort_requested.load()) {
                queue_pcm_to_speaker(stream_ctx.prebuf.data(), stream_ctx.prebuf.size());
                stream_ctx.prebuf.clear();
            }
            flush_silence_and_wait();
            nk_imu_resume();
            s_playing.store(false);
            return !s_abort_requested.load();
        }
    }

    // 2. HTS が使えない場合は WAV 連結にフォールバック
    if (std::strncmp(text, kTriviaPrefix, sizeof(kTriviaPrefix) - 1) == 0) {
        // The mora WAV fallback cannot read the kanji in 知ってる. Keep the
        // opening even before HTS is ready.
        if (!s_abort_requested.load()) {
            play_phrase_wav_fallback("ねえ、しってる。");
        }
        text += sizeof(kTriviaPrefix) - 1;
    }
    play_phrase_wav_fallback(text);
    nk_imu_resume();
    s_playing.store(false);
    return !s_abort_requested.load();
}

enum VoiceMsgType {
    VOICE_MSG_SAY = 0,
    VOICE_MSG_SAY_WAV_ONLY,
    VOICE_MSG_TEST_QUANT,
};

struct VoiceMessage {
    VoiceMsgType type;
    uint8_t quant_mode;
    uint32_t ticket = 0;
    bool exact = false;
    bool diag = false;      // シリアルの say: フィラーで飛ばさず必ず合成し、計測のログを出す
    bool verbose = false;   // say -v
    int64_t t_req_us = 0;
    char text[128];
};

struct QuantStreamContext {
    uint8_t quant_mode = 0;
    HtsPlayStreamCtx base_ctx;
    // IMA-ADPCM 復号・追跡用状態
    int16_t adpcm_pred = 0;
    int8_t adpcm_idx = 0;
};

static void apply_quantization_to_samples(int16_t *samples, size_t count, uint8_t quant_mode,
                                         int16_t &adpcm_pred, int8_t &adpcm_idx) {
    if (quant_mode == 0) return; // 16-bit PCM: そのまま

    static const int16_t kStepTab[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
        19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
        130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
        5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
    };
    static const int8_t kIdxTab[16] = {
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8
    };

    if (quant_mode == 1) {
        // 8-bit PCM (下位8bit切り捨て & 8bit量子化ノイズシミュレーション)
        for (size_t i = 0; i < count; ++i) {
            int8_t s8 = (int8_t)(samples[i] >> 8);
            samples[i] = ((int16_t)s8) << 8;
        }
    } else if (quant_mode == 2) {
        // 4-bit IMA-ADPCM (エンコード -> 即時デコード往復シミュレーション)
        for (size_t i = 0; i < count; ++i) {
            int16_t s16 = samples[i];
            int16_t step = kStepTab[adpcm_idx];
            int16_t diff = s16 - adpcm_pred;
            uint8_t sign = 0;
            if (diff < 0) { sign = 8; diff = -diff; }
            uint8_t nibble = 0;
            if (diff >= step) { nibble |= 4; diff -= step; }
            step >>= 1;
            if (diff >= step) { nibble |= 2; diff -= step; }
            step >>= 1;
            if (diff >= step) { nibble |= 1; }
            nibble |= sign;

            // ADPCM デコード
            int16_t step_d = kStepTab[adpcm_idx];
            int16_t diff_d = step_d >> 3;
            if (nibble & 1) diff_d += (step_d >> 2);
            if (nibble & 2) diff_d += (step_d >> 1);
            if (nibble & 4) diff_d += step_d;
            if (nibble & 8) adpcm_pred -= diff_d;
            else adpcm_pred += diff_d;
            adpcm_pred = (int16_t)std::clamp<int32_t>(adpcm_pred, -32768, 32767);
            adpcm_idx = (int8_t)std::clamp<int>(adpcm_idx + kIdxTab[nibble], 0, 88);
            samples[i] = adpcm_pred;
        }
    }
}

static bool hts_stream_quant_consumer_cb(const int16_t *samples, size_t count,
                                         size_t total_estimated_samples, void *user_ctx) {
    auto *qctx = static_cast<QuantStreamContext *>(user_ctx);
    if (s_abort_requested.load()) return false;

    // 量子化変換用の一時バッファにコピーして量子化
    std::vector<int16_t> qbuf(samples, samples + count);
    apply_quantization_to_samples(qbuf.data(), count, qctx->quant_mode,
                                  qctx->adpcm_pred, qctx->adpcm_idx);

    return hts_stream_consumer_cb(qbuf.data(), count, total_estimated_samples, &qctx->base_ctx);
}

static void do_play_test_quant(uint8_t quant_mode, const char *text) {
    s_abort_requested.store(false);
    s_playing.store(true);
    nk_imu_suspend();

    const char *phrase = (text != nullptr && text[0] != '\0') ? text : "うん、そうだね";
    ESP_LOGI(kTag, "Play test quant: mode=%u text='%s'", (unsigned)quant_mode, phrase);

    if (sano_voice_is_ready()) {
        QuantStreamContext qctx;
        qctx.quant_mode = quant_mode;
        qctx.base_ctx.t_request = (uint32_t)(esp_timer_get_time() / 1000);
        esp_err_t err = sano_voice_synthesize_text_stream(phrase, hts_stream_quant_consumer_cb, &qctx);
        if (err == ESP_OK) {
            if (!qctx.base_ctx.prebuf.empty() && !s_abort_requested.load()) {
                queue_pcm_to_speaker(qctx.base_ctx.prebuf.data(), qctx.base_ctx.prebuf.size());
                qctx.base_ctx.prebuf.clear();
            }
            flush_silence_and_wait();
            nk_imu_resume();
            s_playing.store(false);
            return;
        }
        ESP_LOGW(kTag, "HTS quant test synth failed (%s), fallback to WAV", esp_err_to_name(err));
    }

    // HTS 未初期化時は WAV フォールバック
    play_phrase_wav_fallback(phrase);
    flush_silence_and_wait();
    nk_imu_resume();
    s_playing.store(false);
}

void voice_task(void *arg) {
    VoiceMessage msg;
    while (true) {
        bool received = false;
        {
            VoiceAudioGuard guard;
            // Claim a dequeue under the same lock as microphone ownership.
            // Blocking here would keep listen_acquire waiting on an idle queue.
            if (!s_listen_owned && !s_model_write_owned && xQueueReceive(s_voice_queue, &msg, 0) == pdTRUE) {
                s_worker_busy.store(true);
                received = true;
            }
        }
        if (received) {
            if (msg.type == VOICE_MSG_SAY) {
                if (!msg.ticket || s_tracked.load() == (msg.ticket | NK_SPEECH_PENDING)) {
                    const bool completed = play_phrase(msg.text, msg.exact, msg.diag, msg.verbose, msg.t_req_us);
                    if (msg.ticket) {
                        VoiceAudioGuard guard;
                        // Publish DONE only after the worker becomes available;
                        // a quiz may immediately enqueue its next question.
                        s_worker_busy.store(false);
                        auto expected = msg.ticket | NK_SPEECH_PENDING;
                        s_tracked.compare_exchange_strong(expected, msg.ticket |
                            (completed ? NK_SPEECH_DONE : NK_SPEECH_CANCELLED));
                    }
                }
            } else if (msg.type == VOICE_MSG_SAY_WAV_ONLY) {
                s_abort_requested.store(false);
                s_playing.store(true);
                nk_imu_suspend();
                play_phrase_wav_fallback(msg.text);
                nk_imu_resume();
                s_playing.store(false);
            } else if (msg.type == VOICE_MSG_TEST_QUANT) {
                do_play_test_quant(msg.quant_mode, msg.text);
            }
            s_worker_busy.store(false);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

} // namespace

esp_err_t voice_player_init(void) {
    struct InitDone {
        ~InitDone() { s_voice_init_done.store(true); }
    } init_done;
    if (!s_fs_mounted) {
        const esp_vfs_littlefs_conf_t conf = {
            .base_path = kMountPath,
            .partition_label = "littlefs",
            .partition = NULL,
            .format_if_mount_failed = false,
            .read_only = true,
            .dont_mount = false,
            .grow_on_mount = false,
        };
        esp_err_t err = esp_vfs_littlefs_register(&conf);
        if (err != ESP_OK) {
            // LittleFS が無くても続ける（WAV フォールバックは使えない。フィラーは合成）
            ESP_LOGE(kTag, "failed to mount LittleFS: %s (続行)", esp_err_to_name(err));
        } else {
            s_fs_mounted = true;

            size_t total = 0, used = 0;
            esp_littlefs_info("littlefs", &total, &used);
            ESP_LOGI(kTag, "LittleFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
        }
    }

    // DLS-020: HTS エンジン初期化 (LittleFS からモデルを PSRAM にロード)
    sano_voice_set_boundary_cb(sano_boundary_cb);
    sano_voice_set_progress_cb(nk_boot_progress);
    esp_err_t hts_err = sano_voice_init();
    show_boot_voice_name();
    if (hts_err != ESP_OK) {
        ESP_LOGW(kTag, "sano_voice_init failed (%s), will use WAV fallback", esp_err_to_name(hts_err));
    } else {
        sano_fillers_preload();
    }

    if (s_voice_queue == nullptr) {
        if (!s_audio_mutex) s_audio_mutex = xSemaphoreCreateMutex();
        if (!s_audio_mutex) return ESP_ERR_NO_MEM;
        s_voice_queue = xQueueCreate(4, sizeof(VoiceMessage));
        if (!s_voice_queue) return ESP_ERR_NO_MEM;
        // コア0で優先度3（サンプリングタスクより低く、BLE/UIタスクと同等）で実行
        // HTS 合成とフロントエンド処理のためスタックを 32KB に拡大
        // 失敗を黙って見逃すと、発話のキューが空のまま誰も取り出さなくなる。原因（内部 DRAM の断片化）が分かる値を出す。
        const BaseType_t rc = xTaskCreatePinnedToCore(voice_task, "nk_voice", nk::kVoiceStackBytes, nullptr, 3, nullptr, 0);
        if (rc != pdPASS) {
            ESP_LOGE(kTag, "nk_voice task create failed: rc=%d stack=%u B, internal largest free block=%u B",
                     (int)rc, (unsigned)nk::kVoiceStackBytes,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

nk_speech_status_t voice_player_tracked_status(void) { return static_cast<nk_speech_status_t>(s_tracked.load() & 3u); }
bool voice_player_init_finished(void) { return s_voice_init_done.load(); }

static bool say_tracked_locked(const char *text, bool exact) {
    if (!text || !*text || !s_voice_queue || std::strlen(text) >= 128
        || (s_listen_owned && !s_listen_standby) || s_model_write_owned || s_worker_busy.load() || voice_player_is_playing()
        || uxQueueMessagesWaiting(s_voice_queue) != 0
        || voice_player_tracked_status() == NK_SPEECH_PENDING) return false;
    VoiceMessage msg{};
    msg.type = VOICE_MSG_SAY;
    s_next_speech_ticket += 4;
    if (!s_next_speech_ticket) s_next_speech_ticket = 4;
    msg.ticket = s_next_speech_ticket;
    msg.exact = exact;
    std::strcpy(msg.text, text);
    s_tracked.store(msg.ticket | NK_SPEECH_PENDING);
    if (xQueueSend(s_voice_queue, &msg, 0) != pdTRUE) {
        s_tracked.store(msg.ticket | NK_SPEECH_CANCELLED);
        return false;
    }
    return true;
}

nk_say_result_t voice_player_say_diag(const char *text, bool verbose) {
    VoiceAudioGuard guard;
    if (!text || !*text) return NK_SAY_EMPTY;
    if (std::strlen(text) >= sizeof(VoiceMessage::text)) return NK_SAY_TOO_LONG;
    if (!s_voice_queue || !sano_voice_is_ready()) return NK_SAY_NOT_READY;
    if (s_listen_owned && !s_listen_standby) return NK_SAY_LISTENING;
    if (s_model_write_owned) return NK_SAY_MODEL_WRITE;
    if (s_worker_busy.load() || voice_player_is_playing() || uxQueueMessagesWaiting(s_voice_queue) != 0
        || voice_player_tracked_status() == NK_SPEECH_PENDING) return NK_SAY_BUSY;
    VoiceMessage msg{};
    msg.type = VOICE_MSG_SAY;
    msg.exact = true; msg.diag = true; msg.verbose = verbose;
    msg.t_req_us = esp_timer_get_time();
    std::strcpy(msg.text, text);
    return xQueueSend(s_voice_queue, &msg, 0) == pdTRUE ? NK_SAY_OK : NK_SAY_QUEUE_FULL;
}

const char *voice_player_say_result_text(nk_say_result_t r) {
    switch (r) {
    case NK_SAY_OK: return "受け付けました";
    case NK_SAY_EMPTY: return "文が空です";
    case NK_SAY_TOO_LONG: return "文が長すぎます（127 B まで）";
    case NK_SAY_NOT_READY: return "声が使えません（起動中、または声・辞書の読み込みに失敗しています）";
    case NK_SAY_LISTENING: return "聞き取り中です";
    case NK_SAY_MODEL_WRITE: return "モデル／辞書の書き込み中です（辞書の書き込みの後は、再起動するまでこの状態が続きます）";
    case NK_SAY_BUSY: return "発話中です（終わってからもう一度送ってください）";
    case NK_SAY_QUEUE_FULL: return "発話の待ち行列がいっぱいです";
    }
    return "不明";
}

bool voice_player_say_tracked(const char *text, bool exact) {
    VoiceAudioGuard guard;
    return say_tracked_locked(text, exact);
}

static bool listen_acquire_locked(bool standby) {
    if (!s_audio_mutex || !s_voice_queue || s_listen_owned || s_model_write_owned || s_worker_busy.load()
        || s_playing.load() || M5.Speaker.isPlaying()
        || uxQueueMessagesWaiting(s_voice_queue) != 0
        || voice_player_tracked_status() == NK_SPEECH_PENDING) return false;
    s_listen_owned = true;
    s_listen_standby = standby;
    s_listen_yield.store(false);
    return true;
}

bool voice_player_listen_acquire(void) {
    VoiceAudioGuard guard;
    return listen_acquire_locked(false);
}

bool voice_player_listen_acquire_standby(void) {
    VoiceAudioGuard guard;
    return listen_acquire_locked(true);
}

void voice_player_listen_release(void) {
    VoiceAudioGuard guard;
    s_listen_owned = false;
    s_listen_standby = false;
    s_listen_yield.store(false);
}

bool voice_player_listen_yield_requested(void) { return s_listen_yield.load(); }

bool voice_player_speech_waiting(void) {
    VoiceAudioGuard guard;
    return s_voice_queue && uxQueueMessagesWaiting(s_voice_queue) != 0;
}

bool voice_player_model_write_acquire(void) {
    VoiceAudioGuard guard;
    if (s_listen_owned && s_listen_standby) {
        s_listen_yield.store(true);
        return false;
    }
    if (!s_voice_init_done.load() || !s_audio_mutex || !s_voice_queue || s_model_write_owned || s_listen_owned
        || s_worker_busy.load() || s_playing.load() || M5.Speaker.isPlaying()
        || uxQueueMessagesWaiting(s_voice_queue) != 0
        || voice_player_tracked_status() == NK_SPEECH_PENDING) return false;
    s_model_write_owned = true;
    return true;
}

void voice_player_model_write_release(void) {
    VoiceAudioGuard guard;
    s_model_write_owned = false;
}

bool voice_player_listen_reply(const char *text) {
    VoiceAudioGuard guard;
    if (!s_listen_owned) return false;
    s_listen_owned = false;
    return say_tracked_locked(text, true);
}

bool voice_player_audio_lock(bool listen_owner) {
    // A cue while the wake-word standby holds the microphone: ask it to release, then wait (bounded) instead of dropping the cue.
    int64_t deadline = 0;
    for (;;) {
        if (s_audio_mutex) xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
        if (!listen_owner && s_listen_owned && s_listen_standby && !s_model_write_owned) {
            s_listen_yield.store(true);
            if (s_audio_mutex) xSemaphoreGive(s_audio_mutex);
            const int64_t now = esp_timer_get_time();
            if (!deadline) deadline = now + 2500000;
            if (now >= deadline) return false;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (s_model_write_owned || s_listen_owned != listen_owner) {
            if (s_audio_mutex) xSemaphoreGive(s_audio_mutex);
            return false;
        }
        return true;
    }
}

void voice_player_audio_unlock(void) {
    if (s_audio_mutex) xSemaphoreGive(s_audio_mutex);
}

bool voice_player_say(const char *text) {
    VoiceAudioGuard guard;
    if ((s_listen_owned && !s_listen_standby) || s_model_write_owned) return false;
    if (text == nullptr || *text == '\0' || s_voice_queue == nullptr) {
        return false;
    }
    VoiceMessage msg;
    if (std::strlen(text) >= sizeof(msg.text)) return false;
    msg.type = VOICE_MSG_SAY;
    msg.quant_mode = 0;
    std::strncpy(msg.text, text, sizeof(msg.text) - 1);
    msg.text[sizeof(msg.text) - 1] = '\0';

    cancel_tracked();
    if (s_playing.load()) {
        s_abort_requested.store(true);
        M5.Speaker.stop(kVoiceChannel);
        xQueueReset(s_voice_queue);
    }

    return xQueueSend(s_voice_queue, &msg, 0) == pdTRUE;
}

bool voice_player_is_playing(void) {
    return s_playing.load() || M5.Speaker.isPlaying(kVoiceChannel);
}

bool voice_player_mouth_open(void) {
    if (M5.Speaker.getVolume() == 0 || !M5.Speaker.isPlaying(kVoiceChannel)) return false;
    const uint32_t tick = static_cast<uint32_t>(esp_timer_get_time() / kEnvelopePeriodUs);
    const auto &slot = s_envelope[tick % kEnvelopeSlots];
    return slot.tick.load(std::memory_order_acquire) == tick
        && slot.level.load(std::memory_order_relaxed) >= 500;
}

void voice_player_stop(void) {
    VoiceAudioGuard guard;
    if (s_listen_owned) return;
    cancel_tracked();
    s_abort_requested.store(true);
    M5.Speaker.stop(kVoiceChannel);
    if (s_voice_queue != nullptr) {
        xQueueReset(s_voice_queue);
    }
}

void voice_player_set_volume(uint8_t volume) {
    M5.Speaker.setVolume(volume);
}

uint8_t voice_player_get_volume(void) {
    return M5.Speaker.getVolume();
}

// DLS-027: 量子化別音声再生テスト (0=16-bit PCM, 1=8-bit PCM, 2=4-bit IMA-ADPCM)

void voice_player_play_test_quant(uint8_t quant_mode, const char *text) {
    VoiceAudioGuard guard;
    if ((s_listen_owned && !s_listen_standby) || s_model_write_owned) return;
    if (s_voice_queue == nullptr) return;
    VoiceMessage msg;
    msg.type = VOICE_MSG_TEST_QUANT;
    msg.quant_mode = quant_mode;
    if (text != nullptr) {
        std::strncpy(msg.text, text, sizeof(msg.text) - 1);
        msg.text[sizeof(msg.text) - 1] = '\0';
    } else {
        msg.text[0] = '\0';
    }

    cancel_tracked();
    if (s_playing.load()) {
        s_abort_requested.store(true);
        M5.Speaker.stop(kVoiceChannel);
        xQueueReset(s_voice_queue);
    }

    xQueueSend(s_voice_queue, &msg, 0);
}

void voice_player_say_wav_only(const char *text) {
    VoiceAudioGuard guard;
    if ((s_listen_owned && !s_listen_standby) || s_model_write_owned) return;
    if (text == nullptr || *text == '\0' || s_voice_queue == nullptr) return;
    VoiceMessage msg;
    msg.type = VOICE_MSG_SAY_WAV_ONLY;
    msg.quant_mode = 0;
    std::strncpy(msg.text, text, sizeof(msg.text) - 1);
    msg.text[sizeof(msg.text) - 1] = '\0';

    cancel_tracked();
    if (s_playing.load()) {
        s_abort_requested.store(true);
        M5.Speaker.stop(kVoiceChannel);
        xQueueReset(s_voice_queue);
    }

    xQueueSend(s_voice_queue, &msg, 0);
}
