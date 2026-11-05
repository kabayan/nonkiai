#include "direction_ir.h"
#include "direction_ir_wave.h"

#include <atomic>
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ble_stream.h"
#include "task_stack_sizes.h"

namespace {
const char *const kTag = "nk-ir";
struct Request {
    uint8_t direction;
    uint32_t id;
    uint32_t generation;
    bool automatic;
    bool hold;
    uint8_t mode;
    uint8_t count;
    uint16_t interval_ms;
};
constexpr int64_t kNecMinPeriodUs = 108080;
QueueHandle_t s_queue = nullptr;
rmt_channel_handle_t s_tx = nullptr;
rmt_encoder_handle_t s_encoder = nullptr;
std::atomic<bool> s_busy{false};
std::atomic<esp_err_t> s_ready{ESP_ERR_INVALID_STATE};

esp_err_t transmit_frame(uint8_t direction, int64_t *start_us = nullptr)
{
    // Retain symbol storage even if wait_all_done reports a driver error.
    // The shared worker is the only writer of this buffer.
    static rmt_symbol_word_t symbols[34] = {};
    symbols[0].level0 = 1;
    symbols[0].duration0 = 8960;
    symbols[0].duration1 = 4480;
    for (unsigned i = 0; i < 32; ++i) {
        symbols[i + 1].level0 = 1;
        symbols[i + 1].duration0 = 560;
        symbols[i + 1].duration1 = nk_ir_bit_space(nk_ir_saved_value(direction), i);
    }
    symbols[33].level0 = 1;
    symbols[33].duration0 = 560;
    symbols[33].duration1 = 1;
    rmt_transmit_config_t config = {};
    config.flags.eot_level = 0;
    if (start_us) *start_us = esp_timer_get_time();
    esp_err_t err = rmt_transmit(s_tx, s_encoder, symbols, sizeof(symbols), &config);
    if (err == ESP_OK) {
        // Wait for RMT to finish before this worker can reuse the symbols.
        err = rmt_tx_wait_all_done(s_tx, -1);
        if (err != ESP_OK && start_us) s_ready.store(ESP_ERR_INVALID_STATE);
    }
    return err;
}

esp_err_t transmit_repeat(int64_t *start_us)
{
    // NEC short repeat: 9 ms mark, 2.25 ms space, 560 us mark, then LOW.
    static rmt_symbol_word_t symbols[2] = {};
    symbols[0].level0 = 1;
    symbols[0].duration0 = 9000;
    symbols[0].duration1 = 2250;
    symbols[1].level0 = 1;
    symbols[1].duration0 = 560;
    symbols[1].duration1 = 1;
    rmt_transmit_config_t config = {};
    config.flags.eot_level = 0;
    *start_us = esp_timer_get_time();
    esp_err_t err = rmt_transmit(s_tx, s_encoder, symbols, sizeof(symbols), &config);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_tx, -1);
        if (err != ESP_OK) s_ready.store(ESP_ERR_INVALID_STATE);
    }
    return err;
}

void wait_until_us(int64_t deadline)
{
    // Anchor each start to an absolute clock. A delayed worker never catches
    // up by shortening the next interval. The sub-tick tail prevents early TX.
    const int64_t tick_us = 1000000 / configTICK_RATE_HZ;
    for (;;) {
        const int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) return;
        if (remaining >= tick_us) {
            vTaskDelay(static_cast<TickType_t>(remaining / tick_us));
        } else {
            esp_rom_delay_us(static_cast<uint32_t>(remaining));
        }
    }
}

esp_err_t send_hold(const Request &request, uint8_t *sent)
{
    *sent = 0;
    int64_t baseline = 0;
    int64_t last_start = 0;
    const int64_t interval_us = static_cast<int64_t>(request.interval_ms) * 1000;
    const int64_t minimum_interval_us = interval_us > kNecMinPeriodUs
        ? interval_us : kNecMinPeriodUs;
    for (uint8_t index = 0; index < request.count; ++index) {
        if (index != 0) {
            const int64_t scheduled = baseline + static_cast<int64_t>(index) * minimum_interval_us;
            const int64_t minimum = last_start + minimum_interval_us;
            wait_until_us(scheduled > minimum ? scheduled : minimum);
        }
        int64_t start = 0;
        const esp_err_t err = (index == 0 || request.mode == 1)
            ? transmit_frame(request.direction, &start) : transmit_repeat(&start);
        if (index == 0) baseline = start;
        last_start = start;
        if (err != ESP_OK) return err;
        ++*sent;
    }
    if (request.mode == 1 || request.count == 1) {
        // A final full frame keeps the original 50 ms plus tick margin LOW.
        const TickType_t gap = (50U * configTICK_RATE_HZ + 999U) / 1000U;
        vTaskDelay(gap + 1);
    }
    // A short final repeat needs substantially more than 50 ms LOW. Hold
    // the worker until at least one complete NEC period from its start.
    wait_until_us(last_start + kNecMinPeriodUs);
    return ESP_OK;
}

void worker(void *)
{
    Request request;
    for (;;) {
        if (xQueueReceive(s_queue, &request, portMAX_DELAY) != pdTRUE) continue;
        uint8_t sent = 0;
        const esp_err_t err = send_hold(request, &sent);
        if (request.hold) {
            ESP_LOGI(kTag, "TX hold request=%lu direction=%u mode=%u count=%u interval_ms=%u sent=%u result=%s",
                     static_cast<unsigned long>(request.id), request.direction, request.mode,
                     request.count, request.interval_ms, sent, esp_err_to_name(err));
        } else {
            ESP_LOGI(kTag, "TX %s request=%lu direction=%u value=0x%08lX count=%u sent=%u result=%s",
                     request.automatic ? "auto" : "manual",
                     static_cast<unsigned long>(request.id), request.direction,
                     static_cast<unsigned long>(nk_ir_saved_value(request.direction)),
                     request.count, sent, esp_err_to_name(err));
        }
        s_busy.store(false);
        // Automatic sends are not WebUI test results; the page must not see them.
        if (!request.automatic) {
            nk_ble_notify_ir_result(request.direction, request.id, err, request.generation);
        }
    }
}

esp_err_t enqueue(const Request &item)
{
    if (item.direction >= 4) return ESP_ERR_INVALID_ARG;
    const esp_err_t ready = s_ready.load();
    if (ready != ESP_OK) return ready;
    if (s_busy.exchange(true)) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        s_busy.store(false);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
} // namespace

esp_err_t nk_ir_init(void)
{
    rmt_tx_channel_config_t channel = {};
    channel.gpio_num = GPIO_NUM_2;
    channel.clk_src = RMT_CLK_SRC_DEFAULT;
    channel.resolution_hz = 1000000;
    channel.mem_block_symbols = 64;
    channel.trans_queue_depth = 1;
    esp_err_t err = rmt_new_tx_channel(&channel, &s_tx);
    if (err != ESP_OK) { s_ready.store(err); return err; }
    rmt_carrier_config_t carrier = {};
    carrier.frequency_hz = 38000;
    carrier.duty_cycle = 0.33f;
    err = rmt_apply_carrier(s_tx, &carrier);
    rmt_copy_encoder_config_t encoder = {};
    if (err == ESP_OK) err = rmt_new_copy_encoder(&encoder, &s_encoder);
    if (err == ESP_OK) err = rmt_enable(s_tx);
    const bool enabled = err == ESP_OK;
    if (err == ESP_OK) {
        s_queue = xQueueCreate(1, sizeof(Request));
        if (!s_queue) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK && xTaskCreate(worker, "nk_ir", nk::kIrStackBytes, nullptr, 2, nullptr) != pdPASS)
        err = ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        if (s_queue) { vQueueDelete(s_queue); s_queue = nullptr; }
        if (enabled) rmt_disable(s_tx);
        if (s_encoder) { rmt_del_encoder(s_encoder); s_encoder = nullptr; }
        rmt_del_channel(s_tx);
        s_tx = nullptr;
    }
    s_ready.store(err);
    ESP_LOGI(kTag, "external GPIO2 NEC TX init: %s", esp_err_to_name(err));
    return err;
}

esp_err_t nk_ir_request(uint8_t direction, uint32_t request, uint32_t generation)
{
    return enqueue({direction, request, generation, false, false, 1,
                    nk_ir_default_repeat(direction), 108});
}

esp_err_t nk_ir_request_hold(uint8_t direction, uint8_t mode, uint8_t count,
                             uint16_t interval_ms, uint32_t request, uint32_t generation)
{
    if (direction >= 4 || mode > 1 || count < 1 || count > 30 ||
        interval_ms < 108 || interval_ms > 500) return ESP_ERR_INVALID_ARG;
    return enqueue({direction, request, generation, false, true, mode, count, interval_ms});
}

esp_err_t nk_ir_request_auto(uint8_t direction)
{
    return enqueue({direction, 0, 0, true, false, 1, nk_ir_default_repeat(direction), 108});
}
