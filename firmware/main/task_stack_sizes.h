#pragma once

#include <stdint.h>

namespace nk {
// ESP-IDF task creation and stack high-water marks both use bytes.
// Share these values between task creation and the serial diagnostics.
// 24,576: 32,768 は HTS 時代の値（DLS-020）。sanoTTS では起動のフィラー事前合成を含めても残りが 14 KB 以上ある（stt32 の作業での実測 min_free 14,348）。
// 内部 DRAM の最大連続領域が 31,744 B のとき、32,768 では作れず、声が一切出なかった（DLS-166）。
constexpr uint32_t kVoiceStackBytes = 24576;
constexpr uint32_t kSamplerStackBytes = 4096;
constexpr uint32_t kIrStackBytes = 3072;
constexpr uint32_t kBootSpeechStackBytes = 4096;
constexpr uint32_t kShadowStackBytes = 6144;
constexpr uint32_t kListenStackBytes = 16384;
constexpr uint32_t kWakeStackBytes = 8192; // 起動語の待受（stt32 DLS-1082）
constexpr uint32_t kModelWriterStackBytes = 6144;
constexpr uint32_t kSerialSayStackBytes = 4096;
// 30d878c stt32 defaults (unchanged since dd10e50); diagnostics only, do not override upstream creation.
constexpr uint32_t kStt32PumpStackBytes = 4096;
constexpr uint32_t kStt32InferStackBytes = 12288;
constexpr uint32_t kStt32WorkerStackBytes = 8192;
} // namespace nk
