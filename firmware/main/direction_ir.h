#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t nk_ir_init(void);
// DLS-147: nonblocking full-frame repetition (forward/backward 5, right/left 4)
// at a minimum 108,080 us start interval. One result after all TX and final LOW.
esp_err_t nk_ir_request(uint8_t direction, uint32_t request, uint32_t connection_generation);
// Nonblocking hold test: mode 0 sends one full frame then NEC repeats; mode 1
// sends only full frames. count includes the first frame. One final BLE result.
esp_err_t nk_ir_request_hold(uint8_t direction, uint8_t mode, uint8_t count,
                             uint16_t interval_ms, uint32_t request,
                             uint32_t connection_generation);
// DLS-147: automatic send after a voice reply uses the same default repetition,
// worker and busy guard, without BLE results. Busy/unavailable is not retried.
esp_err_t nk_ir_request_auto(uint8_t direction);
