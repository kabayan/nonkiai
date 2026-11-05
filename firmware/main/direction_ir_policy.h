#pragma once

#include <stdint.h>

// DLS-143 / stt32 DLS-1082: 音声で聞き取った方向を、返事の発話が正常に終わった後に自動でIR送信するかの判定。
// enabled は認識モードがおもちゃ（nk_listen_mode_sends_ir）であること。
// ホスト試験できるよう副作用を持たない。intent 0..3 (前進/後退/右/左) は
// direction_ir_wave.h の方向番号 (0=hrf, 1=hrb, 2=hrr, 3=hrl) とそのまま一致する。
constexpr uint8_t NK_IR_AUTO_NONE = 0xFF;

// 認識時点で呼ぶ。返事を作れて本体が受理し、設定がONのときだけ方向を持ち越す。
static inline uint8_t nk_ir_auto_latch(uint8_t intent, bool reply_built, bool accepted, bool enabled)
{
    return (reply_built && accepted && enabled && intent < 4) ? intent : NK_IR_AUTO_NONE;
}

// 発話の完了時に呼ぶ。正常完了かつ現在もONのときだけ、持ち越した方向を返す。
static inline uint8_t nk_ir_auto_decide(uint8_t latched, bool completed, bool enabled)
{
    return (completed && enabled && latched < 4) ? latched : NK_IR_AUTO_NONE;
}
