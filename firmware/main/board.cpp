#include "board.h"

#include <M5Unified.hpp>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *const kTag = "nk-board";

void nk_board_init(void)
{
    auto cfg = M5.config();
    cfg.internal_imu = true;
    // Grove GPIO1/2 belongs to the external IR unit, not an I2C peripheral.
    cfg.external_imu = false;
    cfg.external_rtc = false;
    cfg.internal_mic = false;
    cfg.internal_spk = false;  // AtomS3R 本体はスピーカーを持たない

    // Voice Base (Atomic Echo Base) の ES8311 + NS4168 をスピーカーとして使う。
    // 収録の誘導音は PC ではなくおもちゃ側から鳴らす (DLS-008)。
    // M5Unified は AtomS3R でこの経路を I2S BCK=8 / WS=6 / DOUT=5 に割り当てる。
    // internal_imu との併用は M5Unified 側で織り込み済み (ATOMIC SPK と違い
    // atomic_echo は internal_imu を落とさない)。
    cfg.external_speaker.atomic_echo = true;

    M5.begin(cfg);
    ESP_LOGI(kTag, "M5 begun: board=%d", (int)M5.getBoard());

    // DLS-031: M5.Speaker の再生タスク (spk_task) の優先度を既定の 2 から 4 に上げる。
    // nk_voice タスク (優先度3・core0) より上にすることで、HTS 合成 (MLPG/Vocoder の
    // Core1 ワーカーは優先度5) で両コアが埋まっても再生タスクが飢餓しないようにする。
    // core1 の IMU サンプラ (優先度6) より下なので、収録中の IMU からは奪わない。
    // ピン等 atomic_echo の設定はここで一度 M5.begin() 済みの Speaker.config() を
    // 読み直して task_priority だけ書き換えるので壊さない。task_pinned_core は既定
    // (コア指定なし) のまま変更しない。
    // spk_task は M5.Speaker.begin() が呼ばれて初めて生成される（nk_audio_init() 内、
    // この関数より後）ため、その前に config() を書き換えておけば新しい優先度で
    // 生成される。
    auto spk_cfg = M5.Speaker.config();
    spk_cfg.task_priority = 4;
    M5.Speaker.config(spk_cfg);

    // 起動時点ではまだ spk_task は生成されていない想定（-1 になるのが正常）。
    // 実際に生成された後の値は nk_audio_init() 完了後や発話時の [PIPELINE START]
    // ログ側で確認する。
    TaskHandle_t spk_handle = xTaskGetHandle("spk_task");
    const int spk_prio = (spk_handle != nullptr) ? (int)uxTaskPriorityGet(spk_handle) : -1;
    ESP_LOGI(kTag, "spk_task priority after config update (pre-begin, -1 = not created yet): %d", spk_prio);
}
