#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Voice Base (Atomic Echo Base) のスピーカーを起動する。
// nk_board_init() の後、かつサンプリングタスクの起動前に呼ぶこと。
bool nk_audio_init(void);

bool nk_audio_ready(void);

// 誘導音を鳴らす。非同期 (M5Unified のスピーカータスクに積むだけで戻る)。
// freq2_hz が 0 なら単音。0 でなければ 2 音を隙間なく連続再生する
// (「ぴぽ」のような 2 音の方が、おもちゃを持った状態で開始合図と識別しやすい)。
bool nk_audio_beep(uint16_t freq1_hz, uint16_t ms1, uint16_t freq2_hz, uint16_t ms2);

// Reserved for voice_listen while it owns the shared audio port.
bool nk_audio_beep_listen(uint16_t freq1_hz, uint16_t ms1, uint16_t freq2_hz, uint16_t ms2);

#ifdef __cplusplus
}
#endif
