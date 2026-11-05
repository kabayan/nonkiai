// nonkiact2 データサンプリング基盤 (P1) + オンデバイス判定 (P4) + 音声合成 (P5)
#include <cstring>
#include <cstdio>

#include <M5Unified.hpp>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "audio_cue.h"
#include "boot_progress.h"
#include "display_style.h"
#include "display_orb.h"
#include "ble_stream.h"
#include "board.h"
#include "device_weather.h"
#include "direction_ir.h"
#include "imu_sampler.h"
#include "voice_player.h"
#include "serial_say.h"
#include "voice_config.h"
#include "nk_event_core.h"
#include "voice_listen.h"
#include "sano_voice.h"
#include "task_stack_sizes.h"

static const char *const kTag = "nk-main";

struct EyeOffset {
    int x;
    int y;
};

static EyeOffset eye_glance_offset(uint32_t now_ms, bool enabled)
{
    constexpr uint32_t kMoveMs = 300;
    constexpr int kDistanceX = 6;
    constexpr int kDistanceY = 3;
    static bool initialized = false;
    static bool was_enabled = false;
    static bool moving = false;
    static uint32_t next_ms = 0;
    static uint32_t started_ms = 0;
    static uint32_t hold_ms = 0;
    static int direction = 1;
    static int vertical_direction = 0;

    // Restart the quiet interval after sleep or a happy expression.
    if (!initialized || enabled != was_enabled) {
        initialized = true;
        was_enabled = enabled;
        moving = false;
        next_ms = now_ms + 4000 + esp_random() % 5001;
    }
    if (!enabled) return {0, 0};
    if (!moving) {
        if (static_cast<int32_t>(now_ms - next_ms) < 0) return {0, 0};
        moving = true;
        started_ms = now_ms;
        hold_ms = 800 + esp_random() % 801;
        // Horizontal only 60%, vertical only 20%, diagonal 20%.
        const uint32_t kind = esp_random() % 5;
        direction = kind == 3 ? 0 : ((esp_random() & 1) ? 1 : -1);
        vertical_direction = kind < 3 ? 0 : ((esp_random() & 1) ? 1 : -1);
    }

    const uint32_t elapsed = now_ms - started_ms;
    if (elapsed >= 2 * kMoveMs + hold_ms) {
        moving = false;
        next_ms = now_ms + 4000 + esp_random() % 5001;
        return {0, 0};
    }
    const uint32_t progress = elapsed < kMoveMs ? elapsed
        : (elapsed < kMoveMs + hold_ms ? kMoveMs
           : 2 * kMoveMs + hold_ms - elapsed);
    return {
        direction * static_cast<int>((progress * kDistanceX + kMoveMs / 2) / kMoveMs),
        vertical_direction * static_cast<int>((progress * kDistanceY + kMoveMs / 2) / kMoveMs),
    };
}

static void draw_face(bool force = false)
{
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const bool asleep = nk_imu_is_sleeping();
    const uint32_t tap_ms = nk_imu_last_tap_ms();
    const bool happy = !asleep && tap_ms != 0 && now_ms - tap_ms < 800;
    static bool last_asleep = false;
    static uint32_t next_blink_ms = 0;
    static uint32_t blink_until_ms = 0;
    if (next_blink_ms == 0 || asleep != last_asleep) {
        last_asleep = asleep;
        next_blink_ms = now_ms + 2500 + esp_random() % 3500;
        blink_until_ms = 0;
    }
    if (!asleep && !happy && static_cast<int32_t>(now_ms - next_blink_ms) >= 0) {
        blink_until_ms = now_ms + 160;
        next_blink_ms = now_ms + 2500 + esp_random() % 3500;
    }
    const bool blinking = !asleep && !happy && static_cast<int32_t>(blink_until_ms - now_ms) > 0;
    const EyeOffset eyes = eye_glance_offset(now_ms, !asleep && !happy);
    const bool mouth_open = voice_player_mouth_open();
    const uint8_t breath = asleep ? static_cast<uint8_t>((now_ms / 650) % 4) : 0;
    const uint32_t frame = (static_cast<uint32_t>(eyes.y + 3) << 24)
                         | (static_cast<uint32_t>(eyes.x + 6) << 20)
                         | (static_cast<uint32_t>(asleep) << 16)
                         | (static_cast<uint32_t>(happy) << 12)
                         | (static_cast<uint32_t>(mouth_open) << 8)
                         | (static_cast<uint32_t>(blinking) << 4) | breath;
    static uint32_t last_frame = UINT32_MAX;
    if (!force && frame == last_frame) return;
    last_frame = frame;

    const int w = M5.Display.width();
    const int h = M5.Display.height();
    const int cx = w / 2;
    const int eye_cx = cx + eyes.x;
    const int cy = h / 2 + (asleep ? (breath == 0 ? 0 : (breath == 2 ? 2 : 1)) : 0);
    const int eye_cy = cy + eyes.y;
    const uint16_t ink = TFT_WHITE;
    M5.Display.startWrite();
    M5.Display.fillScreen(TFT_BLACK);
    if (asleep || blinking) {
        M5.Display.drawLine(eye_cx - 29, eye_cy - 6, eye_cx - 13, eye_cy - 6, ink);
        M5.Display.drawLine(eye_cx + 13, eye_cy - 6, eye_cx + 29, eye_cy - 6, ink);
        M5.Display.drawLine(eye_cx - 26, eye_cy - 4, eye_cx - 16, eye_cy - 9, ink);
        M5.Display.drawLine(eye_cx + 16, eye_cy - 9, eye_cx + 26, eye_cy - 4, ink);
    } else if (happy) {
        M5.Display.drawLine(cx - 29, cy - 4, cx - 21, cy - 12, ink);
        M5.Display.drawLine(cx - 21, cy - 12, cx - 13, cy - 4, ink);
        M5.Display.drawLine(cx + 13, cy - 4, cx + 21, cy - 12, ink);
        M5.Display.drawLine(cx + 21, cy - 12, cx + 29, cy - 4, ink);
    } else {
        M5.Display.fillCircle(eye_cx - 21, eye_cy - 7, 7, ink);
        M5.Display.fillCircle(eye_cx + 21, eye_cy - 7, 7, ink);
    }
    if (mouth_open) {
        M5.Display.fillRect(cx - 11, cy + 17, 22, 18, ink);
    } else if (happy) {
        M5.Display.drawLine(cx - 14, cy + 19, cx, cy + 29, ink);
        M5.Display.drawLine(cx, cy + 29, cx + 14, cy + 19, ink);
    } else {
        M5.Display.drawLine(cx - 11, cy + 23, cx + 11, cy + 23, ink);
    }
    M5.Display.endWrite();
}

// ---- 起動時のメモリの空きのログ ----
#include "esp_heap_caps.h"

static void log_mem_line(const char *tag)
{
    const uint32_t cap_i = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(kTag, "[MEM %s] internal free=%u largest=%u min_free=%u | psram free=%u largest=%u",
             tag,
             (unsigned)heap_caps_get_free_size(cap_i),
             (unsigned)heap_caps_get_largest_free_block(cap_i),
             (unsigned)heap_caps_get_minimum_free_size(cap_i),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

// boot_speech can delete itself on the other core. Protect its handle until
// the stack scan finishes; name lookup alone cannot keep a task alive.
static portMUX_TYPE s_boot_stack_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_boot_speech_task = nullptr;

static void log_stack_lines(bool force)
{
    struct TaskStack {
        const char *name;
        uint32_t size;
        UBaseType_t last_min_free;
    };
    static TaskStack tasks[] = {
        {"main", CONFIG_ESP_MAIN_TASK_STACK_SIZE, UINT32_MAX},
        {"nk_voice", nk::kVoiceStackBytes, UINT32_MAX},
        {"nk_wake", nk::kWakeStackBytes, UINT32_MAX},
        {"nk_sampler", nk::kSamplerStackBytes, UINT32_MAX},
        {"nk_ir", nk::kIrStackBytes, UINT32_MAX},
        {"nk_model", nk::kModelWriterStackBytes, UINT32_MAX},
        {"nk_serial", nk::kSerialSayStackBytes, UINT32_MAX},
        {"boot_speech", nk::kBootSpeechStackBytes, UINT32_MAX},
        {"nk_shadow", nk::kShadowStackBytes, UINT32_MAX},
        {"stt32_pump", nk::kStt32PumpStackBytes, UINT32_MAX},
        {"stt32_infer", nk::kStt32InferStackBytes, UINT32_MAX},
        {"stt32_wh_w1", nk::kStt32WorkerStackBytes, UINT32_MAX},
        {"nimble_host", CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE, UINT32_MAX},
    };
    constexpr size_t kTaskCount = sizeof(tasks) / sizeof(tasks[0]);
    UBaseType_t min_free[kTaskCount];
    bool decreased = false;
    for (size_t i = 0; i < kTaskCount; ++i) {
        min_free[i] = UINT32_MAX; // absent tasks must not sample the caller
        if (std::strcmp(tasks[i].name, "boot_speech") == 0) {
            portENTER_CRITICAL(&s_boot_stack_mux);
            if (s_boot_speech_task != nullptr)
                min_free[i] = uxTaskGetStackHighWaterMark(s_boot_speech_task);
            portEXIT_CRITICAL(&s_boot_stack_mux);
        } else {
            // These tasks remain alive for the lifetime of the application.
            const TaskHandle_t task = xTaskGetHandle(tasks[i].name);
            if (task != nullptr) min_free[i] = uxTaskGetStackHighWaterMark(task);
        }
        if (min_free[i] < tasks[i].last_min_free) decreased = true;
    }
    if (!force && !decreased) return;
    for (size_t i = 0; i < kTaskCount; ++i) {
        if (min_free[i] == UINT32_MAX) continue;
        tasks[i].last_min_free = min_free[i];
        ESP_LOGI(kTag, "[STACK] %s size=%u min_free=%u", tasks[i].name,
                 (unsigned)tasks[i].size, (unsigned)min_free[i]);
    }
}

// ---- 起動のプログレスバー（顔が出て最初に発声できるまで）----
static bool s_bar_inited = false;
static int64_t s_bar_us = 0;     // 描画に使った時間の合計
static int s_bar_draws = 0;
static int s_bar_last_pct = -1;

extern "C" void nk_boot_progress(const char *label, int pct)
{
    const int64_t t0 = esp_timer_get_time();
    ESP_LOGI(kTag, "[BOOT] %-12s %3d%% (t=%lld ms)", label, pct, (long long)(t0 / 1000));
    constexpr int kX = 14, kW = 100, kY = 62, kH = 10;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (!s_bar_inited) {
        s_bar_inited = true;
        M5.Display.setRotation(1);
        M5.Display.fillScreen(TFT_BLACK);
        M5.Display.drawRect(kX - 2, kY - 2, kW + 4, kH + 4, TFT_WHITE);
        M5.Display.setTextDatum(middle_center);
        M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        M5.Display.drawString("booting", 64, 44);
    }
    // 進んだ分だけ塗る（巻き戻しは無い）
    const int from = s_bar_last_pct < 0 ? 0 : kW * s_bar_last_pct / 100;
    const int to = kW * pct / 100;
    if (to > from) M5.Display.fillRect(kX + from, kY, to - from, kH, TFT_GREEN);
    s_bar_last_pct = pct;
    M5.Display.fillRect(0, 80, 128, 16, TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString(label, 64, 88);
    s_bar_us += esp_timer_get_time() - t0;
    ++s_bar_draws;
}

extern "C" void nk_boot_voice_name(const char *text)
{
    ESP_LOGI(kTag, "[BOOT] %s", text);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(middle_center);
    char lines[2][128] = {};
    const char *next = text;
    for (auto &line : lines) {
        size_t n = 0;
        while (*next) {
            size_t bytes = 1;
            while (next[bytes] && (static_cast<unsigned char>(next[bytes]) & 0xc0) == 0x80) ++bytes;
            if (n + bytes >= sizeof line - 1) break;
            std::memcpy(line + n, next, bytes);
            line[n + bytes] = 0;
            if (M5.Display.textWidth(line) > 124) {
                line[n] = 0;
                break;
            }
            n += bytes;
            next += bytes;
        }
    }
    if (*next) {
        char *line = lines[1];
        size_t n = std::strlen(line);
        for (;;) {
            line[n] = '~';
            line[n + 1] = 0;
            if (M5.Display.textWidth(line) <= 124 || n == 0) break;
            do { --n; } while (n && (static_cast<unsigned char>(line[n]) & 0xc0) == 0x80);
        }
    }
    M5.Display.fillRect(0, 13, 128, 20, TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString(lines[0], 64, 18);
    M5.Display.drawString(lines[1], 64, 28);
}

// 起動の発話: 「わたしは、のんきちゃんです。どうぞよろしくね！」の 1 文だけ。フィラーなし（exact 経路はフィラーを付けない）
static void boot_speech_task(void *)
{
    portENTER_CRITICAL(&s_boot_stack_mux);
    s_boot_speech_task = xTaskGetCurrentTaskHandle();
    portEXIT_CRITICAL(&s_boot_stack_mux);
    const int64_t t0 = esp_timer_get_time();
    if (!voice_player_say_tracked("わたしは、のんきちゃんです。どうぞよろしくね！", true))
        ESP_LOGW(kTag, "[BOOT] 起動の発話を受け付けなかった");
    vTaskDelay(pdMS_TO_TICKS(300));
    while (voice_player_is_playing()) vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(kTag, "[BOOT] 起動の発話が終わった t=%lld ms（発話の要求から %lld ms）", (long long)(esp_timer_get_time() / 1000),
             (long long)((esp_timer_get_time() - t0) / 1000));
    const auto min_free = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(kTag, "[STACK] boot_speech size=%u min_free=%u",
             (unsigned)nk::kBootSpeechStackBytes, (unsigned)min_free);
    portENTER_CRITICAL(&s_boot_stack_mux);
    s_boot_speech_task = nullptr;
    portEXIT_CRITICAL(&s_boot_stack_mux);
    vTaskDelete(nullptr);
}

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // notif DLS-190: BLE controller は内部 DRAM の連続ブロックを要求する。
    // 重い初期化 (M5.begin / M5GFX / IMU) を先に走らせて DRAM を断片化させると
    // GATT discovery が完走しなくなる。よって NimBLE を最初に上げる。
    nk_weather_init();
    ESP_ERROR_CHECK(nk_ble_init());

    // M5.begin は IMU とスピーカーの両方の前提なので board 側に一本化してある。
    nk_board_init();
    nk_boot_progress("M5 ok", 5);

    const esp_err_t ir_err = nk_ir_init();
    if (ir_err != ESP_OK) ESP_LOGW(kTag, "IR unavailable: %s", esp_err_to_name(ir_err));

    const bool imu_ok = nk_imu_init();
    if (!imu_ok) {
        ESP_LOGE(kTag, "IMU init failed; BLE stays up for diagnosis");
    }

    // 誘導音は Voice Base から鳴らす (DLS-008)。
    if (!nk_audio_init()) {
        ESP_LOGW(kTag, "audio cue unavailable; recording continues without beep");
    }
    nk_boot_progress("SPEAKER ok", 7);

    // P5-2 / P5-3: 音声設定ロード、LittleFS マウントと音声合成タスクの初期化
    nk_voice_config_init();
    nk::event_mode(nk_voice_config_get_listen_mode());
    voice_player_set_volume(nk_voice_config_get_volume());
    nk_imu_set_swap_nod_tilt(nk_voice_config_get_swap_nod_tilt());
    // Reserve stt32's persistent buffers/stacks before sanoTTS builds its
    // filler cache and performs transient frontend allocations in PSRAM.
    log_mem_line("stt32-before");
    const esp_err_t listen_err = nk_voice_listen_init();
    log_mem_line("stt32-after");
    if (listen_err != ESP_OK) ESP_LOGE(kTag, "stt32 init failed: %s", esp_err_to_name(listen_err));
    nk_boot_progress("STT32 init", 8);
    esp_err_t v_err = voice_player_init();
    if (v_err != ESP_OK) {
        ESP_LOGE(kTag, "voice_player_init failed: %s", esp_err_to_name(v_err));
    }
    // DLS-159: USB シリアルから say <文> で喋らせる入口（失敗しても他の機能は続ける）
    const esp_err_t say_err = nk_serial_say_init();
    if (say_err != ESP_OK) ESP_LOGW(kTag, "nk_serial_say_init failed: %s", esp_err_to_name(say_err));

    if (imu_ok && !nk_imu_start_task()) {
        ESP_LOGE(kTag, "sampler task creation failed");
    }

    M5.Display.setRotation(1);
    M5.Display.fillScreen(TFT_BLACK);
    // HTS synthesis runs at priority 3 on core 0. The default main priority
    // is 1, which leaves the face frozen throughout the lead filler.
    // Share priority 3 with synthesis, below the priority-4 speaker task.
    vTaskPrioritySet(nullptr, 3);

    log_mem_line("boot");
    log_stack_lines(true);
    int64_t last_stack_check_us = esp_timer_get_time();
    ESP_LOGI(kTag, "[BOOT] 準備完了、顔を出す t=%lld ms / プログレスバーの描画 %d 回で合計 %lld us",
             (long long)(esp_timer_get_time() / 1000), s_bar_draws, (long long)s_bar_us);
    if (v_err == ESP_OK) xTaskCreatePinnedToCore(boot_speech_task, "boot_speech", nk::kBootSpeechStackBytes, nullptr, 2, nullptr, 1);
    // stt32 DLS-1082: 起きている間の起動語の待受。起動の発話が済むまで入らない。
    if (listen_err == ESP_OK && v_err == ESP_OK) {
        const esp_err_t wake_err = nk_voice_listen_start_standby();
        if (wake_err != ESP_OK) ESP_LOGE(kTag, "wake standby not started: %s", esp_err_to_name(wake_err));
    }

    uint8_t display_rotation = 1;
    bool first_face = true;
    uint8_t style = nk_voice_config_get_display_style();
    bool force_redraw = true;
    uint32_t last_draw_ms = static_cast<uint32_t>(esp_timer_get_time()/1000);
    for (;;) {
        M5.update();
        const uint32_t button_ms = static_cast<uint32_t>(esp_timer_get_time()/1000);
        if (M5.BtnA.wasPressed()) {
            nk_imu_note_button(button_ms, true);
        }
        // M5Unified hold sets _press=2; release clicks only when _press was 1.
        if (M5.BtnA.wasHold()) {
            ESP_LOGI(kTag, "BtnA hold -> trigger TTS test");
            voice_player_say("こんにちは、のんきちゃんだよ");
        }
        if (M5.BtnA.wasClicked()) {
            style = nk_display_style_next(style);
            nk_voice_config_set_display_style(style);
            if (style == static_cast<uint8_t>(DisplayStyle::Orb)) nk_display_orb_reset_failure();
            force_redraw = true;
            ESP_LOGI(kTag, "BtnA click -> style=%u, trigger TTS test", (unsigned)style);
            voice_player_say("こんにちは、のんきちゃんだよ");
        }
        if (M5.BtnA.wasReleased()) nk_imu_note_button(button_ms, false);
        if (button_ms - last_draw_ms < 50) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        last_draw_ms = button_ms;
        const uint8_t rotation = nk_display_style_is_face(style)
            ? (1 + nk_display_style_face_quarter(style)) % 4 : 1;
        if (rotation != display_rotation) {
            M5.Display.setRotation(rotation);
            display_rotation = rotation;
            force_redraw = true;
        }
        bool draw_fallback = false;
        if (style == static_cast<uint8_t>(DisplayStyle::Orb)) {
            const bool asleep = nk_imu_is_sleeping();
            const uint32_t tap_ms = nk_imu_last_tap_ms();
            const bool happy = !asleep && tap_ms != 0 && button_ms-tap_ms < 800;
            if (!nk_display_orb_draw(button_ms, asleep, happy, voice_player_mouth_open(),
                    nk::orb_state(voice_player_is_playing(), nk_voice_listen_sampling(), nk_voice_listen_capturing()),
                    nk_imu_posture_state(), nk_imu_posture_angle_cdeg())) {
                draw_fallback = true;
            }
        }
        if (nk_display_style_is_face(style) || draw_fallback) draw_face(force_redraw);
        force_redraw = false;
        if (first_face) {
            first_face = false;
            ESP_LOGI(kTag, "[BOOT] 顔が出た t=%lld ms", (long long)(esp_timer_get_time() / 1000));
        }
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_stack_check_us >= 10000000) {
            last_stack_check_us = now_us;
            log_stack_lines(false);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
