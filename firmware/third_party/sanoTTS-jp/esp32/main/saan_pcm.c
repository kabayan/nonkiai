#include "saan_pcm.h"

#include <math.h>

/* ⚠️ **リトルエンディアンの 2 バイトを順に食う。** ホスト（esp32/host_stub）と
 *    ターゲットで同じ順序・同じ幅でないと比較が無意味になる。 */
#define FNV_OFFSET 1469598103934665603ull
#define FNV_PRIME  1099511628211ull

/* 軟リミッタ（ピークだけを tanh で抑える）。式の正本は
 * .workflows/model-builder/gain-rule-20261017/gain_rule.py:
 *   |x| <= T : x（恒等。bit 単位で従来と同じ）
 *   |x| >  T : sign(x) * (T + (1-T) * tanh((|x|-T)/(1-T)))   → 出力は常に 1 以下（float では x≳5 で tanh が 1.0f に飽和し ちょうど 1）
 * SAAN_SOFT_LIMIT = T（既定 0.8f）。0 で無効（従来の挙動と完全一致）。0 < T < 1 のみ有効。 */
#ifndef SAAN_SOFT_LIMIT
#define SAAN_SOFT_LIMIT 0.8f
#endif

static uint32_t s_clips;
static uint32_t s_limited;
static uint64_t s_pcm_fnv = FNV_OFFSET;
static uint32_t s_pcm_n;
static int32_t  s_pcm_absmax;
static uint64_t s_pcm_sqsum;

#define LIM_ON (SAAN_SOFT_LIMIT > 0.0f && SAAN_SOFT_LIMIT < 1.0f)

/* 純関数（統計を触らない）。数えるのは saan_f32_to_i16 だけ */
float saan_soft_limit(float x) {
    /* 定数条件（プリプロセッサは浮動小数を比べられない）。無効時はコンパイラが丸ごと消す */
    if (SAAN_SOFT_LIMIT > 0.0f && SAAN_SOFT_LIMIT < 1.0f) {
        const float T = (float)SAAN_SOFT_LIMIT;
        const float a = fabsf(x);
        if (a > T) {
            const float y = T + (1.0f - T) * tanhf((a - T) / (1.0f - T));
            return (x < 0.0f) ? -y : y;
        }
    }
    return x;
}

int16_t saan_f32_to_i16(float x) {
    if (LIM_ON && fabsf(x) > (float)SAAN_SOFT_LIMIT) ++s_limited;
    x = saan_soft_limit(x);
    long v = lrintf(x * 32767.0f);
    if (v > 32767) { v = 32767; ++s_clips; }
    else if (v < -32768) { v = -32768; ++s_clips; }
    uint16_t u = (uint16_t)(int16_t)v;
    s_pcm_fnv = (s_pcm_fnv ^ (uint8_t)(u & 0xff)) * FNV_PRIME;
    s_pcm_fnv = (s_pcm_fnv ^ (uint8_t)(u >> 8)) * FNV_PRIME;
    { int32_t av = (int32_t)(v < 0 ? -v : v);
      if (av > s_pcm_absmax) s_pcm_absmax = av;
      s_pcm_sqsum += (uint64_t)((int64_t)v * (int64_t)v); }
    ++s_pcm_n;
    return (int16_t)v;
}

void saan_pcm_reset(void) {
    s_pcm_fnv = FNV_OFFSET;
    s_pcm_n = 0;
    s_pcm_absmax = 0;
    s_pcm_sqsum = 0;
    s_clips = 0;
    s_limited = 0;
}

uint32_t saan_pcm_clip_count(void) { return s_clips; }
uint32_t saan_pcm_limit_count(void) { return s_limited; }
uint64_t saan_pcm_checksum(void)   { return s_pcm_fnv; }
uint32_t saan_pcm_samples(void)    { return s_pcm_n; }
int32_t  saan_pcm_absmax(void)     { return s_pcm_absmax; }
uint64_t saan_pcm_sqsum(void)      { return s_pcm_sqsum; }
