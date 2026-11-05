#pragma once
// 声ごとの値（DLS-314 の f0Shape と DLS-315 の vnoise）を model 領域の最後の 4 KB セクタに置く記録。
// ESP-IDF にも FreeRTOS にも依存しない（ファーム・書き込み器・ホストのテストで同じ実装を使う）。
//
// 記録（96 B、整数と浮動小数点はすべて LE。浮動小数点は IEEE754）
//   0  magic "NKVP"        4  version u16 = 1       6  flags u16 = 0
//   8  model_bytes u32     12 reserved u32 = 0
//   16 model_sha256[32]    （この記録が属するモデルの SHA-256。起動時にモデルを読み直して照合する）
//   48 k f64   56 center_hz f64   64 lo_hz f64   72 hi_hz f64   80 tau_ms f64
//   88 vnoise f32          92 crc32 u32（0..91 の CRC-32/IEEE）
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NK_VP_SECTOR_BYTES 4096u
#define NK_VP_RECORD_BYTES 96u
#define NK_VP_VERSION 1u
#define NK_VP_K_MIN 0.5
#define NK_VP_K_MAX 3.0
#define NK_VP_DEFAULT_VNOISE 0.03f
#define NK_VP_DEFAULT_TAU_MS 75.0

typedef struct {
    double k, center_hz, lo_hz, hi_hz, tau_ms;   // k == 1.0 は整形なし（そのとき center 以降は 0 でもよい）
    float vnoise;
} nk_voice_values_t;

static inline uint32_t nk_vp_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// 値の検査。NaN は比較がすべて偽になるので必ず落ちる。saan_stream_set_f0shape / set_vnoise が受ける範囲と同じ。
static inline bool nk_vp_valid(const nk_voice_values_t *v) {
    if (!(v->k >= 0.0 && v->k <= NK_VP_K_MAX)) return false;
    if (!(v->vnoise > 0.0f && v->vnoise <= 1.0f)) return false;
    if (v->k != 1.0) {
        if (!(v->center_hz > 0.0 && v->lo_hz > 0.0 && v->lo_hz < v->center_hz && v->hi_hz > v->center_hz)) return false;
        if (!(v->tau_ms > 0.0)) return false;
    }
    return true;
}

static inline void nk_vp_put32(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i)); }
static inline uint32_t nk_vp_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void nk_vp_putd(uint8_t *p, double d) {
    uint64_t u; memcpy(&u, &d, 8);
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(u >> (8 * i));
}
static inline double nk_vp_getd(const uint8_t *p) {
    uint64_t u = 0;
    for (unsigned i = 0; i < 8; ++i) u |= (uint64_t)p[i] << (8 * i);
    double d; memcpy(&d, &u, 8); return d;
}

// 記録を作る。値が不正なら false（out は触らない）。
static inline bool nk_vp_encode(uint8_t out[NK_VP_RECORD_BYTES], uint32_t model_bytes, const uint8_t sha[32],
                                const nk_voice_values_t *v) {
    if (!nk_vp_valid(v)) return false;
    uint8_t r[NK_VP_RECORD_BYTES]; memset(r, 0, sizeof r);
    memcpy(r, "NKVP", 4); r[4] = (uint8_t)NK_VP_VERSION;
    nk_vp_put32(r + 8, model_bytes);
    memcpy(r + 16, sha, 32);
    nk_vp_putd(r + 48, v->k); nk_vp_putd(r + 56, v->center_hz); nk_vp_putd(r + 64, v->lo_hz);
    nk_vp_putd(r + 72, v->hi_hz); nk_vp_putd(r + 80, v->tau_ms);
    uint32_t vn; memcpy(&vn, &v->vnoise, 4); nk_vp_put32(r + 88, vn);
    nk_vp_put32(r + 92, nk_vp_crc32(r, 92));
    memcpy(out, r, sizeof r);
    return true;
}

// 記録を読む。magic・版・CRC・値の範囲のどれかが合わなければ false。model_sha は呼び出し側が照合する。
static inline bool nk_vp_decode(const uint8_t in[NK_VP_RECORD_BYTES], uint32_t *model_bytes, uint8_t sha[32],
                                nk_voice_values_t *v) {
    if (memcmp(in, "NKVP", 4) != 0) return false;
    if (in[4] != NK_VP_VERSION || in[5] != 0 || in[6] != 0 || in[7] != 0) return false;
    if (nk_vp_get32(in + 92) != nk_vp_crc32(in, 92)) return false;
    nk_voice_values_t t;
    t.k = nk_vp_getd(in + 48); t.center_hz = nk_vp_getd(in + 56); t.lo_hz = nk_vp_getd(in + 64);
    t.hi_hz = nk_vp_getd(in + 72); t.tau_ms = nk_vp_getd(in + 80);
    uint32_t vn = nk_vp_get32(in + 88); memcpy(&t.vnoise, &vn, 4);
    if (!nk_vp_valid(&t)) return false;
    *model_bytes = nk_vp_get32(in + 8);
    memcpy(sha, in + 16, 32);
    *v = t;
    return true;
}

// Optional name record at sector offset 96; NKVP v1 is unchanged.
// Relative offsets: 0 magic NKVN, 4 length, 5..35 ASCII + zero padding,
// 36 CRC-32/IEEE LE over bytes 0..35 (sector offsets 96..131).
#define NK_VN_RECORD_BYTES 40u
static inline bool nk_vn_encode(uint8_t out[40], const char *name) {
    if (!name) return false;
    size_t n = 0;
    while (n < 32 && name[n]) {
        if ((uint8_t)name[n] < 0x20 || (uint8_t)name[n] > 0x7e) return false;
        ++n;
    }
    if (n == 0 || n > 31) return false;
    uint8_t r[40] = {0};
    memcpy(r, "NKVN", 4); r[4] = (uint8_t)n;
    memcpy(r + 5, name, n);
    nk_vp_put32(r + 36, nk_vp_crc32(r, 36));
    memcpy(out, r, sizeof r);
    return true;
}
static inline bool nk_vn_decode(const uint8_t in[40], char out[32]) {
    if (memcmp(in, "NKVN", 4) || in[4] == 0 || in[4] > 31 ||
        nk_vp_get32(in + 36) != nk_vp_crc32(in, 36)) return false;
    for (unsigned i = 0; i < in[4]; ++i)
        if (in[5 + i] < 0x20 || in[5 + i] > 0x7e) return false;
    for (unsigned i = in[4]; i < 31; ++i) if (in[5 + i] != 0) return false;
    memcpy(out, in + 5, in[4]); out[in[4]] = 0;
    return true;
}

#ifdef __cplusplus
}
#endif
