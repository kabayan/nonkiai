#include "audio_cue.h"
#include "voice_player.h"

#include <M5Unified.hpp>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *const kTag = "nk-audio";

// スピーカー音量は「ユーザーに聞こえる」と「ビープの振動が加速度に乗りすぎない」の
// 妥協点。机上で大きすぎるとユーザーが判断したため 160 から下げた。蓋を閉じた状態の
// 可聴性は P1-11 で確認して再調整する (DLS-008)。
static constexpr uint8_t kVolume = 128;

// 2 音を連結するため、誘導音は常に同じチャンネルに載せる (自動割り当てだと
// 別チャンネルに乗って和音として同時に鳴ってしまう)。
static constexpr int kCueChannel = 0;

// M5.Speaker.begin() 直前の待ち。起動直後にアンプを立ち上げると「ぶっ」というポップ音が出るため、
// 電源が落ち着くのを待ってから begin する（計測時の切り分け用の印の既定がこの位置の 300 ms 待ち）。
static constexpr uint32_t kSpeakerPrebeginDelayMs = 300;

static bool s_ready = false;

bool nk_audio_init(void)
{
    // Speaker の enable コールバックは ES8311 / PI4IOE を I2C で叩き、その間だけ
    // I2C port1 を GPIO38/39 へ差し替える。BMI270 も同じ port を使うので、
    // サンプリングタスクが回り始める前にここで一度 begin しておく。
    // begin 後の tone() は I2S だけで完結し、IMU の I2C と競合しない。
    vTaskDelay(pdMS_TO_TICKS(kSpeakerPrebeginDelayMs));
    if (!M5.Speaker.begin()) {
        ESP_LOGE(kTag, "speaker begin failed");
        s_ready = false;
        return false;
    }
    M5.Speaker.setVolume(kVolume);
    s_ready = true;
    ESP_LOGI(kTag, "speaker ready (vol=%u)", (unsigned)kVolume);
    return true;
}

bool nk_audio_ready(void) { return s_ready; }

static bool beep(uint16_t freq1_hz, uint16_t ms1, uint16_t freq2_hz, uint16_t ms2,
                 bool listen_owner)
{
    if (!s_ready || !voice_player_audio_lock(listen_owner)) {
        return false;
    }
    // 同一チャンネルの 2 スロットを使う。1 音目は stop_current=true で即鳴らし、
    // 2 音目は stop_current=false で裏スロットに積む。スピーカータスクが 1 音目の
    // 再生完了後に continue するので、2 音の間に隙間が入らない。
    if (!M5.Speaker.tone((float)freq1_hz, ms1, kCueChannel, true)) {
        voice_player_audio_unlock();
        return false;
    }
    bool ok = true;
    if (freq2_hz != 0 && ms2 != 0) {
        ok = M5.Speaker.tone((float)freq2_hz, ms2, kCueChannel, false);
    }
    voice_player_audio_unlock();
    return ok;
}

bool nk_audio_beep(uint16_t freq1_hz, uint16_t ms1, uint16_t freq2_hz, uint16_t ms2)
{
    return beep(freq1_hz, ms1, freq2_hz, ms2, false);
}

bool nk_audio_beep_listen(uint16_t freq1_hz, uint16_t ms1, uint16_t freq2_hz, uint16_t ms2)
{
    return beep(freq1_hz, ms1, freq2_hz, ms2, true);
}
