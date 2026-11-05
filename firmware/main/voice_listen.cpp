#include "voice_listen.h"
#include "nk_event_core.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <M5Unified.hpp>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "stt32.h"

#include "audio_cue.h"
#include "ble_stream.h"
#include "device_clock.h"
#include "device_weather.h"
#include "imu_sampler.h"
#include "listen_messages.h"
#include "direction_ir_policy.h"
#include "voice_config.h"
#include "voice_player.h"
#include "task_stack_sizes.h"

namespace {
constexpr char kTag[] = "nk-listen";
constexpr int kSampleRate = 16000;
constexpr size_t kSamples = 48000;
constexpr size_t kPcmChunkSamples = 320; // stt32's 20 ms read_pcm unit
constexpr uint32_t kStopTimeoutMs = 5000;
// stt32 DLS-1091 の推奨値。静かな状態が 500 ms 続いたら窓を閉じる（0 で 3 秒固定）。
constexpr uint16_t kListenEndHoldMs = 500;
// 起動語の受理から合図音までを縮めるため待受は 200 ms。PC の評価は 300 ms まで。DLS-189
constexpr uint16_t kStandbyEndHoldMs = 200;
constexpr float kListenEndMarginDb = 6.0f;
// 起動語は確率 0.1 以上かつ 18 クラス中 2 位以内（stt32 DLS-1092、別の話者 0/40 → 30/40）。DLS-190
constexpr float kWakeMinScore = 0.1f;
constexpr uint32_t kWakeMaxRank = 2;
// 聞き取りも同じ値を使う。
constexpr int kStandbyMicPriority = 7; // stt32 infer is 5, worker 6 (core 1), pump max-1 (core 1)
// Standby: 起動語の待受がマイクを持っている（stt32 DLS-1082）。聞き取り中ではないので nk_voice_listen_busy は false を返す。
enum class ListenState { Idle, Capture, Reply, SilentReply, Failed, Standby };
std::atomic<ListenState> s_state{ListenState::Idle};
std::atomic<uint8_t> s_ir_direction{NK_IR_AUTO_NONE};
std::atomic<bool> s_ready{false}, s_sampling{false}, s_pcm_error{false};
std::atomic<bool> s_result_overflow{false}, s_dump_pcm{false};
std::atomic<uint32_t> s_busy_started_us{0}, s_busy_duration_us{0};
// 起動語の待受（stt32 DLS-1082）。on_busy は infer タスクから呼ばれるので atomic だけを触る。
std::atomic<bool> s_infer_busy{false}, s_yield{false};
std::atomic<bool> s_wake_request{false};
std::atomic<const char *> s_standby_failure{""};
std::atomic<uint32_t> s_wake_infers{0}, s_wake_busy_max_us{0};
QueueHandle_t s_results = nullptr;
int8_t s_previous_ir_request[4] = {-1, -1, -1, -1};
int8_t s_previous_greeting[2] = {-1, -1};
int8_t s_previous_conversation[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
uint8_t s_event;
uint32_t s_seq;
bool s_capture_dump;
// 区間の時刻（DLS-185）。wake の受け渡しは s_wake_request と同じ順序で行う。
static std::atomic<int64_t> s_wake_win_close_us{0}, s_wake_result_us{0}, s_standby_left_us{0};
static std::atomic<bool> s_listen_by_wake{false};
// Mic_Class copies into these buffers; they are not I2S DMA destinations.
int16_t *s_pcm_buffers = nullptr;
size_t s_pcm_next = 0;
bool s_pcm_queued = false;
// Optional development dump; stt32 owns the full recognition window.
int16_t *s_dump_buffer = nullptr;
size_t s_dump_count = 0, s_dump_skip = 0;

static bool wait_speaker(uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    while (M5.Speaker.isPlaying()) {
        if (esp_timer_get_time() >= deadline) {
            M5.Speaker.stop();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

static bool cue(bool start)
{
    const bool ok = nk_audio_beep_listen(start ? 660 : 990, 100, start ? 990 : 660, 150);
    return wait_speaker(1500) && ok;
}

static bool configure_codec()
{
    // Both Mic/Speaker callbacks and these registers remap the shared I2C port.
    m5gfx::i2c::i2c_temporary_switcher_t bus(1, GPIO_NUM_38, GPIO_NUM_39);
    constexpr uint8_t regs[][2] = {{0x14, 0x1A}, {0x16, 0x05}, {0x17, 0xBF}, {0x1C, 0x6A}};
    bool ok = true;
    for (const auto &reg : regs) {
        uint8_t actual = 0;
        if (!m5::In_I2C.writeRegister8(0x18, reg[0], reg[1], 100000)
            || !m5::In_I2C.readRegister(0x18, reg[0], &actual, 1, 100000)
            || actual != reg[1]) {
            ESP_LOGE(kTag, "ES8311 register 0x%02x failed (read=0x%02x)", reg[0], actual);
            ok = false;
            break;
        }
    }
    bus.restore();
    return ok;
}

static esp_err_t read_pcm(void *, int16_t *samples, size_t count)
{
    if (s_pcm_error.load()) return ESP_FAIL;
    if (!samples || count != kPcmChunkSamples) {
        s_pcm_error.store(true);
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_pcm_queued) {
        s_pcm_next = 0;
        s_pcm_queued = true; // flush also owns a partially primed queue on failure
        for (size_t i = 0; i < 2; ++i) {
            if (!M5.Mic.record(s_pcm_buffers + i * count, count, kSampleRate, false)) {
                s_pcm_error.store(true);
                return ESP_FAIL;
            }
        }
    }
    // isRecording() counts pending requests (0/1/2). At most one may remain
    // before the older buffer is safe to copy. Waiting for zero would empty
    // Mic_Class's queue and discard the unconsumed part of its DMA read.
    // Return after one chunk, even on a fault, so listen_stop can park the pump.
    const int64_t deadline = esp_timer_get_time() + int64_t(count) * 1000000 / kSampleRate + 200000;
    while (M5.Mic.isRecording() > 1) {
        if (esp_timer_get_time() >= deadline) {
            s_pcm_error.store(true);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    int16_t *completed = s_pcm_buffers + s_pcm_next * count;
    std::memcpy(samples, completed, count * sizeof(int16_t));
    if (!M5.Mic.record(completed, count, kSampleRate, false)) {
        s_pcm_error.store(true);
        return ESP_FAIL;
    }
    s_pcm_next ^= 1;
    if (s_dump_buffer) {
        const size_t skip = count < s_dump_skip ? count : s_dump_skip;
        s_dump_skip -= skip;
        size_t n = count - skip;
        if (n > kSamples - s_dump_count) n = kSamples - s_dump_count;
        std::memcpy(s_dump_buffer + s_dump_count, samples + skip, n * sizeof(int16_t));
        s_dump_count += n;
    }
    return ESP_OK;
}

static esp_err_t flush_pcm(void *)
{
    // Contract §§6/12: stt32 calls this with the pump parked, at start/stop.
    // Drop both prefetched chunks so no stopped-session PCM can be reused.
    // On a fault leave ownership intact until listen_task joins via Mic.end.
    if (!s_pcm_queued) return ESP_OK;
    const int64_t deadline = esp_timer_get_time() + int64_t(2 * kPcmChunkSamples) * 1000000 / kSampleRate + 200000;
    while (M5.Mic.isRecording()) {
        if (esp_timer_get_time() >= deadline) {
            s_pcm_error.store(true);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    s_pcm_queued = false;
    s_pcm_next = 0;
    return ESP_OK;
}

static void on_result(void *, const stt32_result_t *result)
{
    // Contract §8: value copy only; playback and lifecycle calls stay in nk_listen.
    if (xQueueSend(s_results, result, 0) != pdTRUE) s_result_overflow.store(true);
}

static void on_busy(void *, bool busy)
{
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
    s_infer_busy.store(busy);
    if (busy) {
        s_busy_started_us.store(now);
        if (s_state.load() == ListenState::Standby) s_wake_infers.fetch_add(1);
    } else {
        const uint32_t duration = now - s_busy_started_us.load();
        s_busy_duration_us.store(duration);
        if (s_state.load() == ListenState::Standby && duration > s_wake_busy_max_us.load()) s_wake_busy_max_us.store(duration);
    }
    // No BLE, logging, I2C or wait on the infer task. This independent pause
    // cannot undo an audio pause if false arrives after listen_stop.
    nk_imu_set_inference_busy(busy);
}

static void dump_pcm()
{
    printf("@LISTEN_PCM_BEGIN {\"seq\":%lu,\"samples\":%u,\"rate\":16000,\"format\":\"s16le-hex\"}\n",
           static_cast<unsigned long>(s_seq), static_cast<unsigned>(s_dump_count));
    constexpr size_t chunk_bytes = 256;
    char hex[chunk_bytes * 2 + 1];
    constexpr char digits[] = "0123456789abcdef";
    const auto *bytes = reinterpret_cast<const uint8_t *>(s_dump_buffer);
    for (size_t offset = 0; offset < s_dump_count * sizeof(int16_t); offset += chunk_bytes) {
        const size_t n = s_dump_count * sizeof(int16_t) - offset;
        const size_t count = n < chunk_bytes ? n : chunk_bytes;
        for (size_t i = 0; i < count; ++i) {
            hex[2 * i] = digits[bytes[offset + i] >> 4];
            hex[2 * i + 1] = digits[bytes[offset + i] & 15];
        }
        hex[2 * count] = '\0';
        printf("@LISTEN_PCM %lu %u %s\n", static_cast<unsigned long>(s_seq), static_cast<unsigned>(offset), hex);
        vTaskDelay(1);
    }
    printf("@LISTEN_PCM_END %lu\n", static_cast<unsigned long>(s_seq));
}

static void listen_task(void *)
{
    // 区間の時刻（DLS-185）。取れなかった境目は 0 のまま残す。
    const int64_t task_start = esp_timer_get_time();
    int64_t cue1_cmd = 0, cue1_done = 0, mic_ready = 0, listen_start = 0;
    int64_t result_rx = 0, stop_done = 0, speaker_ready = 0, cue2_cmd = 0, cue2_done = 0;
    const auto timed_cue = [&](bool start) {
        auto &cmd = start ? cue1_cmd : cue2_cmd;
        auto &done = start ? cue1_done : cue2_done;
        cmd = esp_timer_get_time();
        const bool ok = cue(start);
        done = esp_timer_get_time();
        return ok;
    };
    // 認識モードは 1 回だけ読む。受け付ける集合・返事の選び方・IR の持ち越しに同じ値を使い、聞き取り中の切り替えで食い違わせない。
    const uint8_t mode = nk_voice_config_get_listen_mode();
    stt32_result_t result{};
    result.semantic_id = STT32_SEMANTIC_ID_INVALID;
    result.expression_id = STT32_EXPRESSION_ID_INVALID;
    uint32_t session = 0;
    bool have_result = false, mic_touched = false, speaker_ok = true;
    const char *failure = "";
    // Every cue/reply is a voice transaction, even when ONE_UTTERANCE auto-stopped.
    bool stopped = stt32_listen_stop(kStopTimeoutMs) == ESP_OK;
    if (!stopped) {
        failure = "stop-before-cue";
    } else if (!nk_imu_wait_idle(100)) {
        failure = "imu-busy";
    } else if (!timed_cue(true)) {
        failure = "start-cue";
    } else {
        if (s_capture_dump)
            s_dump_buffer = static_cast<int16_t *>(heap_caps_malloc(kSamples * sizeof(int16_t),
                                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_capture_dump && !s_dump_buffer) ESP_LOGW(kTag, "PCM dump allocation failed; listening continues");
        s_dump_count = 0;
        s_dump_skip = kSampleRate * 300 / 1000;
        M5.Speaker.end();
        mic_touched = true;
        auto mic = M5.Mic.config();
        mic.sample_rate = kSampleRate;
        mic.input_channel = m5::input_only_left;
        mic.over_sampling = 1;
        mic.magnification = 2; // M5Unified gain/(oversampling*2): unity.
        mic.noise_filter_level = 0;
        mic.task_pinned_core = 0;
        // 聞き取りも推論（core 0、優先度 5、約 320 ms）の最中に録り続ける。
        // 優先度 2 だと Mic の DMA タスクが飢えて read_pcm が期限を越え、届く前の結果を捨てて pcm-read で終わる（実機 3 回中 3 回、DLS-177）。待受と同じく推論より上にする。
        mic.task_priority = kStandbyMicPriority;
        M5.Mic.config(mic);
        if (!M5.Mic.begin()) {
            failure = "mic-begin";
        } else if (!configure_codec()) {
            failure = "codec-registers";
        } else {
            mic_ready = esp_timer_get_time();
            s_sampling.store(true);
            nk_imu_resume(); // steady-state capture uses I2S only
            stt32_listen_options_t options{};
            options.mode = STT32_LISTEN_ONE_UTTERANCE;
            options.end_hold_ms = kListenEndHoldMs;
            options.end_margin_db = kListenEndMarginDb;
            // 集合外は stt32 が REJECTED（OUT_OF_SET / LOW_SCORE）で返す。確率は集合の中で割り戻さない。
            options.intent_mask = nk_listen_mode_mask(mode);
            options.accept_rule = mode == NK_LISTEN_MODE_TOY ? STT32_ACCEPT_TOP1_IN_SET : STT32_ACCEPT_SET_MAX_SCORE;
            options.set_min_score = kListenPlushMinScore;
            if (stt32_listen_start(&options, &session) != ESP_OK) {
                failure = "listen-start";
            } else {
                listen_start = esp_timer_get_time();
                const int64_t deadline = esp_timer_get_time() + 10000000;
                while (!s_pcm_error.load() && !s_result_overflow.load() && esp_timer_get_time() < deadline) {
                    if (xQueueReceive(s_results, &result, pdMS_TO_TICKS(20)) == pdTRUE
                        && result.session_id == session) {
                        result_rx = esp_timer_get_time();
                        have_result = true;
                        break;
                    }
                }
                if (s_result_overflow.load()) have_result = false;
                if (!have_result) failure = s_pcm_error.load() ? "pcm-read"
                    : s_result_overflow.load() ? "result-queue" : "result-timeout";
            }
        }
        stopped = stt32_listen_stop(kStopTimeoutMs) == ESP_OK;
        stop_done = esp_timer_get_time();
        if (!stopped) failure = "stop-before-reply";
    }

    // 集合外（OUT_OF_SET / SET_MAX_SCORE の LOW_SCORE）は弾いて何もしない: 終了音も返事も IR も出さない（stt32 DLS-1082）。他の不受理は今までどおり。
    const bool out_of_set = have_result && result.decision == STT32_REJECTED
                         && (result.reject_reason == STT32_REJECT_OUT_OF_SET
                             || result.reject_reason == STT32_REJECT_LOW_SCORE);
    s_sampling.store(false);
    nk_imu_suspend();
    // If stop fails, retain audio ownership until reboot: another producer
    // must not use I2S while the pump/infer could still be active.
    if (!stopped || !nk_imu_wait_idle(100)) {
        s_ready.store(false);
        speaker_ok = false;
        have_result = false;
        if (stopped) failure = "imu-restore-busy";
    } else if (mic_touched) {
        M5.Mic.end();
        s_pcm_queued = false;
        s_pcm_next = 0;
        speaker_ok = M5.Speaker.begin();
        speaker_ready = esp_timer_get_time();
        if (!speaker_ok) failure = "speaker-restore";
        else if (!out_of_set && !timed_cue(false)) { failure = "end-cue"; have_result = false; }
    }
    if (stopped) {
        if (s_dump_buffer && s_dump_count) dump_pcm();
        if (s_dump_buffer) heap_caps_free(s_dump_buffer);
        s_dump_buffer = nullptr;
    }
    // Resume before handing the reply to nk_voice, which owns its own pause.
    nk_imu_resume();

    const auto intent = have_result && (result.decision == STT32_SEMANTIC_ONLY
                                       || result.decision == STT32_EXPRESSION_CONFIRMED)
                      ? nk_listen_intent_from_semantic(result.semantic_id) : NK_LISTEN_NONE;
    const uint8_t reason = intent != NK_LISTEN_NONE ? 0 : !have_result ? 3
        : result.decision != STT32_REJECTED || result.reject_reason == STT32_REJECT_INFERENCE_ERROR ? 3
        : result.reject_reason == STT32_REJECT_TOO_SHORT ? 1 : 2;
    printf("@LISTEN {\"seq\":%lu,\"event\":%u,\"session\":%lu,\"decision\":%d,\"reject_reason\":%d,"
           "\"mode\":%u,\"semantic_id\":%u,\"score\":%.9g,\"margin\":%.9g,\"intent\":%u,\"reason\":%u,"
           "\"speech_end_us\":%lld,\"result_us\":%lld,\"speech_to_result_ms\":%.3f,"
           "\"busy_us\":%lu,\"failure\":\"%s\"}\n",
           static_cast<unsigned long>(s_seq), s_event, static_cast<unsigned long>(session),
           static_cast<int>(result.decision), static_cast<int>(result.reject_reason),
           static_cast<unsigned>(mode), result.semantic_id, static_cast<double>(result.semantic_score),
           static_cast<double>(result.semantic_margin),
           static_cast<unsigned>(intent), reason, static_cast<long long>(result.speech_end_us),
           static_cast<long long>(result.result_us), (result.result_us - result.speech_end_us) / 1000.0,
           static_cast<unsigned long>(s_busy_duration_us.load()), failure);

    // 原点だけ us、各境目は原点からの ms（DLS-185）。1 行で出して集計する。
    const int64_t t0 = s_listen_by_wake.load() ? s_wake_win_close_us.load() : task_start;
    char times[15][32];
    const int64_t timestamps[] = {
        s_wake_win_close_us.load(), s_wake_result_us.load(), s_standby_left_us.load(), task_start,
        cue1_cmd, cue1_done, mic_ready, listen_start,
        have_result ? result.speech_end_us : 0, have_result ? result.result_us : 0,
        result_rx, stop_done, speaker_ready, cue2_cmd, cue2_done
    };
    for (size_t i = 0; i < 15; ++i) {
        if (timestamps[i]) snprintf(times[i], sizeof(times[i]), "%.1f", (timestamps[i] - t0) / 1000.0);
        else snprintf(times[i], sizeof(times[i]), "null");
    }
    printf("@LATENCY {\"seq\":%lu,\"by\":\"%s\",\"failure\":\"%s\",\"t0_us\":%lld,"
           "\"wake_win_close\":%s,"
           "\"wake_result\":%s,"
           "\"standby_left\":%s,"
           "\"task_start\":%s,"
           "\"cue1_cmd\":%s,"
           "\"cue1_done\":%s,"
           "\"mic_ready\":%s,"
           "\"listen_start\":%s,"
           "\"win_close\":%s,"
           "\"result\":%s,"
           "\"result_rx\":%s,"
           "\"stop_done\":%s,"
           "\"speaker_ready\":%s,"
           "\"cue2_cmd\":%s,"
           "\"cue2_done\":%s}\n",
           static_cast<unsigned long>(s_seq), s_listen_by_wake ? "wake" : "tap", failure,
           static_cast<long long>(t0), times[0], times[1], times[2], times[3], times[4],
           times[5], times[6], times[7], times[8], times[9], times[10], times[11],
           times[12], times[13], times[14]);

    const char *result_name = intent != NK_LISTEN_NONE ? "accepted" : !have_result ? "error"
        : out_of_set ? "out_of_set" : reason == 1 ? "too_short" : reason == 3 ? "error" : "rejected";
    nk::Event("listen").number("mode", mode).number("intent", intent)
        .text("intent_name", nk_listen_intent_name(intent)).number("decision", result.decision)
        .number("reject_reason", result.reject_reason).number("score", result.semantic_score)
        .text("result", result_name).emit();

    if (out_of_set && speaker_ok) {
        // 返事を話さないので、聞き取りの持ち主は release で手放す。Failed は「返事なし」（sampler は動作を終えて戻る）。
        nk_ble_notify_intent(s_event, s_seq, NK_LISTEN_NONE, reason, NK_LISTEN_NONE, 255, result.semantic_score, 0);
        voice_player_listen_release();
        s_ir_direction.store(NK_IR_AUTO_NONE, std::memory_order_relaxed);
        s_state.store(ListenState::Failed, std::memory_order_release);
        const auto min_free = uxTaskGetStackHighWaterMark(nullptr);
        ESP_LOGI("nk-main", "[STACK] nk_listen size=%u min_free=%u", (unsigned)nk::kListenStackBytes, (unsigned)min_free);
        vTaskDelete(nullptr);
        return;
    }

    char greeting[NK_VOICE_TEXT_MAX_LEN] = {};
    if (intent == NK_LISTEN_NAME) nk_voice_config_build_greeting(greeting, sizeof(greeting));
    const bool send_ir = nk_listen_mode_sends_ir(mode);
    char reply[128] = {};
    bool reply_built;
    const char *configured = send_ir ? nk_voice_config_get_listen_reply(intent) : nullptr;
    if (configured && std::strcmp(configured, kListenIrRequestReplies[intent][0]) != 0) {
        const int written = snprintf(reply, sizeof(reply), "%s", configured);
        reply_built = written >= 0 && static_cast<size_t>(written) < sizeof(reply);
    } else if (send_ir && intent <= NK_LISTEN_LEFT) {
        int index;
        do {
            index = nk_listen_choose_ir_request_index(intent, esp_random(), s_previous_ir_request[intent]);
        } while (index < 0);
        const int written = snprintf(reply, sizeof(reply), "%s", kListenIrRequestReplies[intent][index]);
        reply_built = written >= 0 && static_cast<size_t>(written) < sizeof(reply);
        if (reply_built) s_previous_ir_request[intent] = static_cast<int8_t>(index);
    } else if (mode == NK_LISTEN_MODE_PLUSH && (intent == NK_LISTEN_UP || intent == NK_LISTEN_DOWN)) {
        const uint8_t old_volume = nk_voice_config_get_volume();
        const uint8_t volume = nk_listen_step_volume(old_volume, intent == NK_LISTEN_UP);
        voice_player_set_volume(volume);
        nk_voice_config_set_volume(volume);
        const esp_err_t save_err = nk_voice_config_save_volume();
        if (save_err != ESP_OK) ESP_LOGW(kTag, "volume save failed: %d", static_cast<int>(save_err));
        nk_ble_notify_voice_volume(volume);
        ESP_LOGI(kTag, "listen volume %u -> %u", static_cast<unsigned>(old_volume), static_cast<unsigned>(volume));
        reply_built = nk_listen_build_volume_reply(volume, reply, sizeof(reply));
    } else {
        int index = 0;
        const int conversation_row = nk_listen_conversation_row(intent);
        const unsigned history_mode = mode == NK_LISTEN_MODE_TOY ? NK_LISTEN_MODE_TOY : NK_LISTEN_MODE_PLUSH;
        const bool varied = intent == NK_LISTEN_MORNING || intent == NK_LISTEN_NIGHT;
        if (varied) {
            do {
                index = nk_listen_choose_greeting_index(intent, esp_random(), s_previous_greeting[intent - NK_LISTEN_MORNING]);
            } while (index < 0);
        }
        if (conversation_row >= 0) {
            do {
                index = nk_listen_choose_conversation_index(intent, esp_random(), s_previous_conversation[history_mode][conversation_row]);
            } while (index < 0);
        }
        reply_built = nk_listen_build_reply(intent, nk_clock_now_local_ms(), nk_weather_today_text(),
                                            greeting, reply, sizeof(reply), index);
        if (conversation_row >= 0 && reply_built) s_previous_conversation[history_mode][conversation_row] = static_cast<int8_t>(index);
        if (varied && reply_built) s_previous_greeting[intent - NK_LISTEN_MORNING] = static_cast<int8_t>(index);
    }
    if (!reply_built) std::strcpy(reply, kListenRetry);
    // No runner-up id is exposed by stt32. Keep the 14-byte BLE packet, use
    // the accepted product id as raw1 and leave raw2 unavailable.
    nk_ble_notify_intent(s_event, s_seq, intent, reason, intent, 255, result.semantic_score, 0);
    bool accepted = false;
    const bool silent = reply_built && configured && !reply[0];
    if (speaker_ok && silent) {
        voice_player_listen_release();
        accepted = true;
    } else if (speaker_ok) accepted = voice_player_listen_reply(reply);
    else if (s_ready.load()) voice_player_listen_release();
    nk_ble_notify_motion_speech(s_event, s_seq, reply, accepted);
    s_ir_direction.store(nk_ir_auto_latch(intent, reply_built, accepted, send_ir), std::memory_order_relaxed);
    s_state.store(accepted ? (silent ? ListenState::SilentReply : ListenState::Reply) : ListenState::Failed, std::memory_order_release);
    nk::event_state(accepted ? "reply" : "idle", accepted ? "" : "reply-failed");
    const auto min_free = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI("nk-main", "[STACK] nk_listen size=%u min_free=%u", (unsigned)nk::kListenStackBytes, (unsigned)min_free);
    vTaskDelete(nullptr);
}

// ---- 起動語の常時待受（stt32 DLS-1082）----
// 排他は既存の listen_acquire / release と、このファイルの Speaker.end→Mic.begin / Mic.end→Speaker.begin の切り替えをそのまま使う。
// 足したのは「待受中に発話・cue・タップ・眠りが来たら、推論中でなければ待受が譲る」ことと、「静かになったら取り直す」ことだけ。
struct Standby {
    uint32_t session = 0, enters = 0, leaves = 0, wakes = 0;
    int64_t entered_us = 0, not_before_us = 0, reason_since_us = 0, stats_us = 0;
    uint32_t max_yield_ms = 0, max_busy_wait_ms = 0;
    bool wake_seen = false, dead = false;
    float wake_score = 0;
} s_sb;

static int64_t ms_of(int64_t us) { return us / 1000; }

static void standby_cleanup_audio(bool mic_touched)
{
    // IMU は止めてから I2C を切り替える（listen_task と同じ順）。
    nk_imu_suspend();
    nk_imu_wait_idle(100);
    if (mic_touched) {
        M5.Mic.end();
        s_pcm_queued = false;
        s_pcm_next = 0;
        if (!M5.Speaker.begin()) {
            ESP_LOGE(kTag, "[WAKE] speaker restore failed; voice stays off until reboot");
            s_sb.dead = true;
        }
    }
    nk_imu_resume();
}

static bool standby_enter()
{
    auto expected = ListenState::Idle;
    if (!s_state.compare_exchange_strong(expected, ListenState::Standby)) return false;
    if (!voice_player_listen_acquire_standby()) {
        s_standby_failure.store("audio-busy");
        s_state.store(ListenState::Idle);
        return false;
    }
    const int64_t t0 = esp_timer_get_time();
    const char *failure = nullptr;
    bool mic_touched = false, started = false;
    nk_imu_suspend();
    if (!nk_imu_wait_idle(100)) {
        failure = "imu-busy";
    } else {
        M5.Speaker.end();
        mic_touched = true;
        auto mic = M5.Mic.config();
        mic.sample_rate = kSampleRate;
        mic.input_channel = m5::input_only_left;
        mic.over_sampling = 1;
        mic.magnification = 2;
        mic.noise_filter_level = 0;
        mic.task_pinned_core = 0;
        // 待受は推論（core 0、優先度 5）の最中も録り続ける。Mic の DMA タスクが推論より低い優先度（2）だと飢えて
        // read_pcm がタイムアウトし、PCM が途切れる（実機: 推論 1〜1.5 秒の間に pcm-error）。推論より上にする。
        mic.task_priority = kStandbyMicPriority;
        M5.Mic.config(mic);
        if (!M5.Mic.begin()) failure = "mic-begin";
        else if (!configure_codec()) failure = "codec-registers";
    }
    if (!failure) {
        s_pcm_error.store(false);
        s_result_overflow.store(false);
        xQueueReset(s_results);
        nk_imu_resume(); // steady-state capture uses I2S only
        stt32_listen_options_t options{};
        options.mode = STT32_LISTEN_WAKE;
        options.end_hold_ms = kStandbyEndHoldMs;
        options.end_margin_db = kListenEndMarginDb;
        options.after_wake = STT32_AFTER_WAKE_STOP;
        if (stt32_listen_start(&options, &s_sb.session) != ESP_OK) failure = "listen-start";
        else started = true;
    }
    if (failure) {
        s_standby_failure.store(failure);
        nk::event_state("idle", failure);
        ESP_LOGW(kTag, "[WAKE] standby enter failed: %s", failure);
        if (started) stt32_listen_stop(kStopTimeoutMs);
        standby_cleanup_audio(mic_touched);
        voice_player_listen_release();
        s_state.store(ListenState::Idle);
        s_sb.not_before_us = esp_timer_get_time() + 10000000; // do not spin on a persistent fault
        return false;
    }
    s_standby_failure.store("");
    s_sb.entered_us = esp_timer_get_time();
    s_sb.reason_since_us = 0;
    s_sb.wake_seen = false;
    s_wake_infers.store(0);
    s_wake_busy_max_us.store(0);
    ++s_sb.enters;
    nk::event_state("standby", "");
    ESP_LOGI(kTag, "[WAKE] standby enter #%u t=%lld ms (switch %lld ms)", (unsigned)s_sb.enters,
             (long long)ms_of(s_sb.entered_us), (long long)ms_of(s_sb.entered_us - t0));
    return true;
}

// 推論中でないときだけ呼ぶ。
static void standby_leave(const char *reason)
{
    const int64_t t0 = esp_timer_get_time();
    const bool stopped = stt32_listen_stop(kStopTimeoutMs) == ESP_OK;
    if (!stopped) {
        // 止まらないなら I2S を他へ渡せない。listen_task と同じく所有を保ったまま音声を諦める。
        ESP_LOGE(kTag, "[WAKE] stop failed; keeping the microphone until reboot");
        nk::event_state("idle", "stop-failed");
        s_ready.store(false);
        s_sb.dead = true;
        s_yield.store(false);
        s_state.store(ListenState::Idle);
        return;
    }
    standby_cleanup_audio(true);
    voice_player_listen_release();
    s_yield.store(false);
    const int64_t now = esp_timer_get_time();
    const uint32_t latency = s_sb.reason_since_us ? static_cast<uint32_t>(ms_of(now - s_sb.reason_since_us)) : 0;
    if (latency > s_sb.max_yield_ms) s_sb.max_yield_ms = latency;
    ++s_sb.leaves;
    nk::event_state("idle", reason);
    s_sb.not_before_us = now + 1000000;
    s_state.store(ListenState::Idle);
    ESP_LOGI(kTag, "[WAKE] standby leave #%u reason=%s t=%lld ms (in standby %lld ms, switch %lld ms, "
             "reason->released %u ms, max %u ms, wake inferences %u, busy max %lu us)",
             (unsigned)s_sb.leaves, reason, (long long)ms_of(now), (long long)ms_of(now - s_sb.entered_us),
             (long long)ms_of(now - t0), (unsigned)latency, (unsigned)s_sb.max_yield_ms,
             (unsigned)s_wake_infers.load(), (unsigned long)s_wake_busy_max_us.load());
}

static void standby_service()
{
    stt32_result_t r;
    while (xQueueReceive(s_results, &r, 0) == pdTRUE) {
        if (r.session_id == s_sb.session && r.event == STT32_EVENT_WAKE) {
            s_sb.wake_seen = true;
            s_sb.wake_score = r.semantic_score;
            s_wake_win_close_us = r.speech_end_us;
            s_wake_result_us = r.result_us;
        }
    }
    const int64_t now = esp_timer_get_time();
    const char *reason = s_sb.wake_seen ? "wake"
        : s_yield.load() ? "tap"
        : voice_player_listen_yield_requested() ? "cue"
        : voice_player_speech_waiting() ? "speech"
        : !nk_imu_standby_wanted() ? "sleep-or-quiz"
        : (s_pcm_error.load() || s_result_overflow.load()) ? "pcm-error" : nullptr;
    if (!reason) {
        if (now - s_sb.stats_us >= 30000000) {
            s_sb.stats_us = now;
            ESP_LOGI(kTag, "[WAKE] standby: enters=%u leaves=%u wakes=%u wake inferences=%u busy max=%lu us",
                     (unsigned)s_sb.enters, (unsigned)s_sb.leaves, (unsigned)s_sb.wakes,
                     (unsigned)s_wake_infers.load(), (unsigned long)s_wake_busy_max_us.load());
        }
        return;
    }
    if (!s_sb.reason_since_us) s_sb.reason_since_us = now;
    // 推論の途中で Mic を手放さない。発話・cue は voice のキューに残り、推論が終わってから譲る。
    if (s_infer_busy.load()) return;
    const bool wake = s_sb.wake_seen;
    const float score = s_sb.wake_score;
    standby_leave(reason);
    if (wake && !s_sb.dead) {
        ++s_sb.wakes;
        nk::Event("wake").number("score", score).emit();
        s_standby_left_us.store(esp_timer_get_time());
        s_wake_request.store(true);
        const bool requested = nk_imu_request_listen(false);
        if (!requested) s_wake_request.store(false);
        ESP_LOGI(kTag, "[WAKE] wake word accepted score=%.3f -> listen %s", static_cast<double>(score),
                 requested ? "requested" : "not started (busy, asleep or quiz)");
    }
}

static void standby_step()
{
    if (!s_ready.load() || s_sb.dead) return;
    const auto state = s_state.load();
    if (state == ListenState::Standby) {
        standby_service();
        return;
    }
    if (state != ListenState::Idle || esp_timer_get_time() < s_sb.not_before_us) return;
    if (!nk_imu_standby_wanted() || nk_imu_interaction_busy() || nk_imu_is_suspended()
        || voice_player_speech_waiting()) return;
    standby_enter();
}

static void standby_task(void *)
{
    for (;;) {
        standby_step();
        // Observe blocked idle states here, never in the infer callbacks.
        static const char *last_reason = nullptr;
        const char *reason = nk_voice_listen_block_reason();
        if (s_state.load() == ListenState::Idle && reason != last_reason) nk::event_state("idle", reason);
        last_reason = reason;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
} // namespace

esp_err_t nk_voice_listen_init(void)
{
    if (s_results) return ESP_ERR_INVALID_STATE;
    s_results = xQueueCreate(4, sizeof(stt32_result_t));
    if (!s_results) return ESP_ERR_NO_MEM;
    s_pcm_buffers = static_cast<int16_t *>(heap_caps_malloc(2 * kPcmChunkSamples * sizeof(int16_t),
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_pcm_buffers) {
        vQueueDelete(s_results);
        s_results = nullptr;
        return ESP_ERR_NO_MEM;
    }
    stt32_config_t config{};
    config.read_pcm = read_pcm;
    config.flush_pcm = flush_pcm;
    config.on_result = on_result;
    config.on_busy = on_busy;
    config.weights_partition = "stt32w";
    // 起動語（のんきちゃん）の確率の下限と順位の上限。必ず両方を設定する（min_score だけ 0.1 だと何でも起動する）。
    config.wake_thresholds.min_score = kWakeMinScore;
    config.wake_max_rank = kWakeMaxRank;
    // Zero defaults: GRU copy, PSRAM work buffers, task sizes/priorities/cores,
    // semantic_thresholds = 0 (calibration deferred).
    const esp_err_t err = stt32_init(&config);
    s_ready.store(err == ESP_OK);
    if (err != ESP_OK) {
        heap_caps_free(s_pcm_buffers);
        s_pcm_buffers = nullptr;
        vQueueDelete(s_results);
        s_results = nullptr;
    }
    return err;
}

bool nk_voice_listen_start(uint8_t event, uint32_t seq)
{
    if (!s_ready.load()) return false;
    auto expected = ListenState::Idle;
    if (!s_state.compare_exchange_strong(expected, ListenState::Capture)) {
        // 待受中のタップ: 待受に譲らせ（推論中なら終わるまで）、Idle になってから取り直す。
        if (expected != ListenState::Standby) return false;
        s_yield.store(true);
        const int64_t deadline = esp_timer_get_time() + 3000000;
        while (s_state.load() == ListenState::Standby && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(5));
        expected = ListenState::Idle;
        if (!s_state.compare_exchange_strong(expected, ListenState::Capture)) {
            s_yield.store(false);
            return false;
        }
    }
    if (!voice_player_listen_acquire()) {
        s_state.store(ListenState::Idle);
        return false;
    }
    s_listen_by_wake = s_wake_request.exchange(false);
    if (!s_listen_by_wake.load()) {
        s_wake_win_close_us.store(0);
        s_wake_result_us.store(0);
        s_standby_left_us.store(0);
    }
    nk::EventLine<256>("state").text("state", "capture").text("reason", "")
        .text("by", s_listen_by_wake ? "wake" : "tap").emit();
    s_event = event;
    s_seq = seq;
    s_ir_direction.store(NK_IR_AUTO_NONE, std::memory_order_relaxed);
    s_capture_dump = s_dump_pcm.load();
    s_pcm_error.store(false);
    s_result_overflow.store(false);
    s_busy_duration_us.store(0);
    xQueueReset(s_results);
    nk_imu_suspend();
    if (xTaskCreatePinnedToCore(listen_task, "nk_listen", nk::kListenStackBytes, nullptr, 2, nullptr, 0) != pdPASS) {
        voice_player_listen_release();
        nk_imu_resume();
        s_state.store(ListenState::Idle);
        nk::event_state("idle", "task-create-failed");
        return false;
    }
    return true;
}

const char *nk_voice_listen_state(void) {
    switch (s_state.load()) {
    case ListenState::Standby: return "standby";
    case ListenState::Capture: return "capture";
    case ListenState::Reply: return "reply";
    case ListenState::SilentReply: return "silent-reply";
    default: return "idle";
    }
}
const char *nk_voice_listen_block_reason(void) {
    if (!s_ready.load()) return "not-ready";
    if (s_state.load() != ListenState::Idle) return "";
    if (!nk_imu_standby_wanted()) return "sleep-or-quiz";
    if (voice_player_speech_waiting() || voice_player_is_playing()) return "speech";
    if (nk_imu_interaction_busy()) return "interaction";
    if (nk_imu_is_suspended()) return "imu-suspended";
    if (s_pcm_error.load()) return "pcm-error";
    const char *failure = s_standby_failure.load();
    return *failure ? failure : "transition";
}

bool nk_voice_listen_sampling(void) { return s_sampling.load() && !s_pcm_error.load(); }
bool nk_voice_listen_capturing(void) { return s_state.load() == ListenState::Capture; }
bool nk_voice_listen_busy(void) {
    const auto state = s_state.load();
    return state != ListenState::Idle && state != ListenState::Standby;
}
bool nk_voice_listen_reply_ready(void) {
    const auto state = s_state.load(std::memory_order_acquire);
    return state == ListenState::Reply || state == ListenState::SilentReply || state == ListenState::Failed;
}
bool nk_voice_listen_reply_silent(void) { return s_state.load(std::memory_order_acquire) == ListenState::SilentReply; }
bool nk_voice_listen_reply_queued(void) { return s_state.load() == ListenState::Reply; }
uint8_t nk_voice_listen_ir_direction(void) { return s_ir_direction.load(std::memory_order_relaxed); }
void nk_voice_listen_finish(void) {
    s_ir_direction.store(NK_IR_AUTO_NONE, std::memory_order_relaxed);
    s_state.store(ListenState::Idle);
    nk::event_state("idle", "finished");
}
void nk_voice_listen_set_pcm_dump(bool enabled) { s_dump_pcm.store(enabled); }

esp_err_t nk_voice_listen_start_standby(void)
{
    if (!s_ready.load()) return ESP_ERR_INVALID_STATE;
    s_sb.not_before_us = esp_timer_get_time() + 3000000; // let the boot speech start first
    if (xTaskCreatePinnedToCore(standby_task, "nk_wake", nk::kWakeStackBytes, nullptr, 2, nullptr, 0) != pdPASS) {
        ESP_LOGE(kTag, "nk_wake task create failed (stack %u B)", (unsigned)nk::kWakeStackBytes);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
