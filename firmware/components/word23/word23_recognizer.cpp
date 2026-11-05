#include "word23_recognizer.h"

#include "energy_trim.h"
#include "sts_feature_norm.h"
#include "word23_accel_feature_f2.h"
#include "word23_accel_m2t_model.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <new>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#else
#include <chrono>
#endif

namespace {
static_assert(sizeof(int16_t) == sizeof(short), "F2 requires 16-bit short PCM");
static_assert(STS_G5_CLASS_COUNT == W23_CLASS_COUNT, "fixed v0.4 class count");
static_assert(STS_FEAT_SAMPLE_RATE == W23_SAMPLE_RATE, "fixed PCM sample rate");

int64_t now_us() {
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

float elapsed_ms(int64_t start) {
    return static_cast<float>(now_us() - start) / 1000.0f;
}

struct Storage {
    void *data = nullptr;
    W23ArenaLocation location = W23_ARENA_NONE;

    Storage(size_t bytes, bool internal_first) {
#ifdef ESP_PLATFORM
        const uint32_t first = internal_first ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM;
        const uint32_t second = internal_first ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
        data = heap_caps_aligned_alloc(16, bytes, first | MALLOC_CAP_8BIT);
        uint32_t selected = first;
        if (!data) {
            data = heap_caps_aligned_alloc(16, bytes, second | MALLOC_CAP_8BIT);
            selected = second;
        }
        if (data) location = selected == MALLOC_CAP_INTERNAL ? W23_ARENA_INTERNAL : W23_ARENA_PSRAM;
#else
        (void)internal_first;
        data = std::aligned_alloc(16, (bytes + 15) / 16 * 16);
        if (data) location = W23_ARENA_HOST;
#endif
    }
    ~Storage() {
#ifdef ESP_PLATFORM
        heap_caps_free(data);
#else
        std::free(data);
#endif
    }
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;
};
} // namespace

bool w23_recognize(const int16_t *pcm, int samples, W23Result *result) {
    if (!result) return false;
    *result = {};
    const int64_t began = now_us();
    const auto finish = [&](int error) {
        result->error = error;
        if (error) std::fill_n(result->scores, W23_CLASS_COUNT, 0.0f);
        result->total_ms = elapsed_ms(began);
        return error == 0;
    };
    if (!pcm || samples < STS_FEAT_FRAME_LENGTH || samples > W23_MAX_SAMPLES)
        return finish(1);

    const int64_t trim_began = now_us();
    const TrimBounds bounds = energy_trim(pcm, samples);
    result->trim_ms = elapsed_ms(trim_began);
    result->trim_start = bounds.start;
    result->trim_end = bounds.end;
    result->active_frames = bounds.active_frames;
    result->noise_proxy_dbfs = bounds.noise_dbfs;
    result->trim_threshold_dbfs = bounds.threshold_dbfs;
    result->peak_frame_dbfs = bounds.peak_dbfs;
    result->speech_detected = bounds.active_frames > 0;
    if (!result->speech_detected) return finish(0);

    // Reserve for the complete capture, then reuse this buffer for M2t after F2.
    // The 48,000-sample capture uses 95,360 bytes regardless of trim duration.
    const size_t arena_bytes = std::max(sts_f2_workspace_bytes(samples),
                                      word23_m2t_workspace_bytes());
    Storage arena(arena_bytes, true);
    result->arena_bytes = arena_bytes;
    result->arena_location = arena.location;
    if (!arena.data) return finish(2);

    // F2 already needs about 14 KB of task stack; keep the 10,880-byte feature
    // off that stack and out of persistent storage. PSRAM is preferred here.
    Storage feature_storage(STS_FEAT_DIM * sizeof(float), false);
    if (!feature_storage.data) return finish(2);
    auto *feature = static_cast<float *>(feature_storage.data);
    for (int i = 0; i < STS_FEAT_DIM; ++i) ::new (feature + i) float;

    const int64_t feature_began = now_us();
    const int feature_error = sts_f2_log_mel_feature_i16_with_workspace(
        pcm + bounds.start, bounds.end - bounds.start, feature, arena.data, arena_bytes);
    result->feature_ms = elapsed_ms(feature_began);
    if (feature_error) return finish(3);

    const int64_t normalize_began = now_us();
    sts_f2_normalize_feature(feature, STS_FEATURE_MEAN, STS_FEATURE_STD, feature);
    result->normalize_ms = elapsed_ms(normalize_began);

    const int64_t model_began = now_us();
    const bool model_ok = word23_m2t_predict(feature, STS_FEAT_DIM, result->scores,
        W23_CLASS_COUNT, arena.data, arena_bytes, WORD23_ACCEL_M2D);
    result->model_ms = elapsed_ms(model_began);
    if (!model_ok) return finish(4);

    const int64_t softmax_began = now_us();
    float maximum = result->scores[0];
    for (float score : result->scores) {
        if (!std::isfinite(score)) return finish(5);
        maximum = std::max(maximum, score);
    }
    float sum = 0.0f;
    for (float &score : result->scores) {
        score = std::exp(score - maximum);
        sum += score;
    }
    if (!std::isfinite(sum) || sum <= 0.0f) return finish(5);
    for (float &score : result->scores) score /= sum;
    result->softmax_ms = elapsed_ms(softmax_began);
    return finish(0);
}

bool w23_rank_scores3(const float *scores, uint8_t *raw1, uint8_t *raw2, uint8_t *raw3) {
    if (raw1) *raw1 = W23_NONE;
    if (raw2) *raw2 = W23_NONE;
    if (raw3) *raw3 = W23_NONE;
    if (!scores || !raw1 || !raw2 || !raw3 || raw1 == raw2 || raw1 == raw3 || raw2 == raw3)
        return false;
    for (int i = 0; i < W23_CLASS_COUNT; ++i)
        if (!std::isfinite(scores[i]) || scores[i] < 0.0f || scores[i] > 1.0f) return false;
    // Insertion into a sorted top-3; strict > keeps the lower index on ties.
    int top[3] = {-1, -1, -1};
    for (int i = 0; i < W23_CLASS_COUNT; ++i) {
        int pos = 3;
        while (pos > 0 && (top[pos - 1] < 0 || scores[i] > scores[top[pos - 1]])) --pos;
        if (pos >= 3) continue;
        for (int j = 2; j > pos; --j) top[j] = top[j - 1];
        top[pos] = i;
    }
    *raw1 = static_cast<uint8_t>(top[0]);
    *raw2 = static_cast<uint8_t>(top[1]);
    *raw3 = static_cast<uint8_t>(top[2]);
    return true;
}

bool w23_rank_scores(const float *scores, uint8_t *raw1, uint8_t *raw2) {
    uint8_t third;
    return w23_rank_scores3(scores, raw1, raw2, &third);
}

W23Intent w23_decide(const float *scores, bool speech_detected, bool ok) {
    if (!speech_detected || !ok) return W23_NONE;
    uint8_t first, second, third;
    if (!w23_rank_scores3(scores, &first, &second, &third)) return W23_NONE;
    if (first == W23_UNKNOWN_CLASS)
        return second == W23_RIGHT ? W23_NONE : static_cast<W23Intent>(second);
    if (first == W23_RIGHT && scores[W23_RIGHT] < 0.60f)
        return static_cast<W23Intent>(second == W23_UNKNOWN_CLASS ? third : second);
    return static_cast<W23Intent>(first);
}

const char *w23_arena_name(W23ArenaLocation location) {
    switch (location) {
        case W23_ARENA_INTERNAL: return "internal";
        case W23_ARENA_PSRAM: return "psram";
        case W23_ARENA_HOST: return "host";
        default: return "none";
    }
}
