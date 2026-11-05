#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// 呼び出し元は NimBLE のホストタスク。消去・書き込み・SHA は専用タスクのみ。
esp_err_t nk_model_writer_init(void);
bool nk_model_writer_submit(const uint8_t *data, size_t len, uint32_t generation);
void nk_model_writer_disconnected(uint32_t generation);
bool nk_model_writer_busy(void);
int nk_model_writer_info(char *json, size_t capacity, uint16_t max_data);
void nk_model_writer_status(uint8_t out[20]);

// 完了位置を専用 characteristic で返す。古い接続への通知は捨てる。
void nk_ble_notify_model_state(const uint8_t *data, size_t len, uint32_t generation);
void nk_ble_model_transfer_mode(bool active, uint32_t generation);
