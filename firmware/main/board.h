#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// M5Unified を初期化する (M5.begin)。IMU とスピーカーの両方が M5.config を通るため、
// 個々のサブシステムではなくここで一度だけ呼ぶ。
//
// 呼ぶ順序: nk_ble_init() の後、nk_imu_init() / nk_audio_init() の前。
// (notif DLS-190: BLE controller は内部 DRAM の連続ブロックを要求する。M5.begin は
//  M5GFX を含む重い初期化なので、BLE init より前に置くと GATT discovery が壊れる)
void nk_board_init(void);

#ifdef __cplusplus
}
#endif
