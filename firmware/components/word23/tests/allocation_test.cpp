#include "word23_recognizer.h"
#include "esp_heap_caps.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
enum Failure { NONE, INTERNAL, PSRAM, ALL, FEATURE };
Failure failure = NONE;
int allocation_calls = 0;
std::map<void *, uint32_t> live;
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

extern "C" void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, uint32_t caps) {
    ++allocation_calls;
    require(alignment == 16 && (caps & MALLOC_CAP_8BIT), "invalid allocation alignment/capabilities");
    if (failure == ALL || (failure == INTERNAL && (caps & MALLOC_CAP_INTERNAL)) ||
        (failure == PSRAM && (caps & MALLOC_CAP_SPIRAM)) ||
        (failure == FEATURE && bytes == 2720 * sizeof(float))) return nullptr;
    void *pointer = std::aligned_alloc(alignment, (bytes + alignment - 1) / alignment * alignment);
    if (pointer) live[pointer] = caps;
    return pointer;
}
extern "C" void heap_caps_free(void *pointer) {
    if (!pointer) return;
    require(live.erase(pointer) == 1, "unknown or double-freed storage");
    std::free(pointer);
}
extern "C" int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main() {
    try {
        std::vector<int16_t> pcm(48000);
        for (int i = 16000; i < 19200; ++i)
            pcm[i] = static_cast<int16_t>(6000 * std::sin(2 * 3.14159265358979323846 * i / 16));
        const auto original = pcm;
        W23Result result{};
        require(w23_recognize(pcm.data(), 48000, &result) && result.speech_detected &&
                result.arena_location == W23_ARENA_INTERNAL, "default arena must prefer internal RAM");
        require(live.empty() && allocation_calls == 2 && result.arena_bytes == 95360,
                "successful recognition must release arena and feature");

        failure = INTERNAL;
        require(w23_recognize(pcm.data(), 48000, &result) && result.arena_location == W23_ARENA_PSRAM,
                "arena must fall back to PSRAM");
        require(live.empty(), "PSRAM fallback leaked storage");
        failure = PSRAM;
        require(w23_recognize(pcm.data(), 48000, &result) && result.arena_location == W23_ARENA_INTERNAL,
                "feature must fall back to internal RAM");
        require(live.empty(), "feature fallback leaked storage");

        failure = ALL;
        require(!w23_recognize(pcm.data(), 48000, &result) && result.error == 2 &&
                result.arena_location == W23_ARENA_NONE && live.empty(), "allocation failure must be reported");
        failure = FEATURE;
        require(!w23_recognize(pcm.data(), 48000, &result) && result.error == 2 && live.empty(),
                "feature allocation failure must release arena");
        for (float score : result.scores) require(score == 0, "failure must clear probabilities");

        std::vector<int16_t> silence(48000);
        allocation_calls = 0;
        require(w23_recognize(silence.data(), 48000, &result) && !result.speech_detected &&
                result.arena_location == W23_ARENA_NONE && allocation_calls == 0,
                "silence must succeed even when no heap allocation is available");
        require(pcm == original, "recognition modified caller PCM");
        std::puts("word23 internal/PSRAM fallback and allocation failure cleanup: PASS");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
