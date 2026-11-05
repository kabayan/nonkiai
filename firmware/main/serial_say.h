#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// USB Serial/JTAG（/dev/ttyACM0、115200）から 1 行を読み、`say <文>` / `say -v <文>` で喋らせる（DLS-159）。
// 専用の最低優先度タスク nk_serial が受信待ちで眠るだけ。STT・IMU・BLE は USB シリアルを読まない。
esp_err_t nk_serial_say_init(void);

#ifdef __cplusplus
}
#endif
