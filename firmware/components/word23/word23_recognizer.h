#pragma once

#include <stddef.h>
#include <stdint.h>

// Fixed stt32 v0.4 class order. The unknown model class is not an intent.
enum W23Intent : uint8_t {
    W23_FORWARD = 0,
    W23_BACKWARD = 1,
    W23_RIGHT = 2,
    W23_LEFT = 3,
    W23_NAME = 4,
    W23_WEATHER = 5,
    W23_TIME = 6,
    W23_ENERGY = 7,
    W23_PRAISE = 8,
    W23_NONE = 255,
};

constexpr int W23_CLASS_COUNT = 10;
constexpr int W23_UNKNOWN_CLASS = 9;
constexpr int W23_SAMPLE_RATE = 16000;
// Includes the historical 50,880-sample fresh15 recordings. Live capture is 48,000.
constexpr int W23_MAX_SAMPLES = 51200;

enum W23ArenaLocation : uint8_t {
    W23_ARENA_NONE = 0,
    W23_ARENA_INTERNAL,
    W23_ARENA_PSRAM,
    W23_ARENA_HOST,
};

struct W23Result {
    float scores[W23_CLASS_COUNT]; // Softmax probabilities; zero when skipped/failed.
    bool speech_detected;
    int trim_start;
    int trim_end;
    int active_frames;
    double noise_proxy_dbfs;
    double trim_threshold_dbfs;
    double peak_frame_dbfs;
    float trim_ms;
    float feature_ms;
    float normalize_ms;
    float model_ms;
    float softmax_ms;
    float total_ms;
    W23ArenaLocation arena_location;
    size_t arena_bytes;
    int error; // 0=success, 1=invalid PCM, 2=allocation, 3=feature, 4=model, 5=scores.
};

// Input is mono 16 kHz, 400..W23_MAX_SAMPLES signed samples.
// PCM remains caller-owned and is never modified. True includes a successful
// silence check: speech_detected=false, zero scores, and no feature/model work.
// All temporary storage is released before return. Caller must serialize calls
// on device while the copied F2 tables are initialized for the first time.
bool w23_recognize(const int16_t *pcm, int samples, W23Result *result);

// Stable ranks: equal scores retain the lower fixed class index. False means
// scores are missing/non-finite/outside [0,1]; output ranks then become NONE.
bool w23_rank_scores(const float *scores, uint8_t *raw1, uint8_t *raw2);

// Same as w23_rank_scores plus third place (same stable tie-break).
bool w23_rank_scores3(const float *scores, uint8_t *raw1, uint8_t *raw2, uint8_t *raw3);

// DLS-141/142 (supersedes DLS-136 thresholds). No probability threshold except
// right. Unknown top1: adopt top2, but top2==right -> NONE. Right top1: keep it
// if p>=.60, else adopt top2 (top3 when top2 is unknown). Other known top1 is
// adopted at any probability. Silence/failure/invalid scores -> NONE.
W23Intent w23_decide(const float *scores, bool speech_detected, bool ok);

const char *w23_arena_name(W23ArenaLocation location);
