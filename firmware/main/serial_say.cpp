#include "serial_say.h"

#include <cstdio>
#include <cstring>
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "serial_say_core.h"
#include "task_stack_sizes.h"
#include "voice_player.h"
#include "voice_config.h"
#include "voice_listen.h"
#include "imu_sampler.h"
#include "nk_event_core.h"

namespace {
constexpr char kTag[] = "nk-say";

void reply(const char *message) { ESP_LOGI(kTag, "%s", message); }

void usage() {
    reply("使い方: say <文> で喋らせます。say -v <文> は ids の記号列と分かち書きも出します（UTF-8、漢字可、127 B まで）。"
          "発話ごとに [SAY] の 1 行が出ます。mode toy/plush/0/1 で切替、mode / status で状態を取得。");
}

void handle(char *line) {
    const nk::SayCmd cmd = nk::parse_say_line(line);
    switch (cmd.kind) {
    case nk::kSayEmpty: return;
    case nk::kSayHelp: usage(); return;
    case nk::kSayUnknown: ESP_LOGW(kTag, "知らないコマンドです: %s", line); usage(); return;
    case nk::kSayTooLong: ESP_LOGW(kTag, "文が長すぎます（%u B まで）", (unsigned)nk::kSayTextMax); return;
    case nk::kSayInvalidMode: ESP_LOGW(kTag, "不正なモード: %s", line); return;
    case nk::kSayMode: nk_voice_config_change_listen_mode(cmd.mode); return;
    case nk::kSayStatus:
        nk::Event("status").number("mode", nk_voice_config_get_listen_mode())
            .number("ir_pin", nk_voice_config_get_ir_pin())
            .text("state", nk_voice_listen_state()).text("reason", nk_voice_listen_block_reason())
            .boolean("speaking", voice_player_is_playing()).boolean("standby_wanted", nk_imu_standby_wanted()).emit();
        return;
    case nk::kSaySay: break;
    }
    const nk_say_result_t r = voice_player_say_diag(cmd.text, cmd.verbose);
    if (r == NK_SAY_OK) ESP_LOGI(kTag, "[SAY] 受け付けました%s: 「%s」", cmd.verbose ? "（-v）" : "", cmd.text);
    else ESP_LOGW(kTag, "[SAY] 断りました: %s", voice_player_say_result_text(r));
}

void task(void *) {
    char line[nk::kSayLineMax + 1];
    size_t n = 0;
    bool overflow = false;
    for (;;) {
        uint8_t buffer[64];
        const int got = usb_serial_jtag_read_bytes(buffer, sizeof(buffer), pdMS_TO_TICKS(200));
        for (int i = 0; i < got; ++i) {
            const char c = (char)buffer[i];
            if (c == '\n' || c == '\r') {
                if (overflow) { ESP_LOGW(kTag, "行が長すぎるため捨てました（%u B まで）", (unsigned)nk::kSayLineMax); }
                else if (n > 0) { line[n] = '\0'; handle(line); }
                n = 0; overflow = false;
            } else if (n < nk::kSayLineMax) {
                line[n++] = c;
            } else {
                overflow = true;
            }
        }
    }
}
} // namespace

esp_err_t nk_serial_say_init(void) {
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    // 既定の 256 B では長い行（漢字 127 B = 約 380 B）が欠ける。
    config.rx_buffer_size = 1024;
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK) { ESP_LOGE(kTag, "usb_serial_jtag_driver_install: %s", esp_err_to_name(err)); return err; }
    if (xTaskCreate(task, "nk_serial", nk::kSerialSayStackBytes, nullptr, 1, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(kTag, "シリアル入力を開始: say <文> / say -v <文> / help");
    return ESP_OK;
}
