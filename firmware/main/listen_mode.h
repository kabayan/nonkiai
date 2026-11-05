#pragma once

#include <stdint.h>

// 認識モード（stt32 DLS-1080 / stt32 DLS-1082）。設定画面・BLE・NVS の番号。聞き取る意図の集合（listen_intents.h）と、
// 前進・後退・右・左を返事のあとに IR で送るかを決める。NVS の読み書きだけが使えるよう stt32 に依存させない。
enum NkListenMode : uint8_t { NK_LISTEN_MODE_PLUSH = 0, NK_LISTEN_MODE_TOY = 1, NK_LISTEN_MODE_COUNT };

inline constexpr bool nk_listen_mode_valid(uint8_t mode) { return mode < NK_LISTEN_MODE_COUNT; }
// おもちゃモードだけが、前進・後退・右・左を返事のあとに IR で送る（旧 IR 自動送信 ON と同じ動作）。
inline constexpr bool nk_listen_mode_sends_ir(uint8_t mode) { return mode == NK_LISTEN_MODE_TOY; }
