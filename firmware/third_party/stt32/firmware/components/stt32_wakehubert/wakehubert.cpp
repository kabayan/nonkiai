// 17-intent recognition core for stt32 (DLS-242/243). Inference part of the verified AtomS3
// firmware (feature/word-stt-23: atoms3_wakehubert_padfix_fwtest_20261025/main/main.cpp), with the
// USB protocol, oprintf, mode-1 injection and app_main removed. Differences from that file:
//   - every work buffer and table is allocated once in init() (cfg->work_caps, 16-byte aligned);
//   - the worker task is created in init() (cfg->worker_core / worker_priority) and stopped in deinit();
//   - weights come from .rodata (incbin) or from a flash partition (cfg->weights_partition), and are
//     partly or wholly copied into cfg->work_caps memory at init (cfg->weights_copy);
//   - trim_bounds finding no speech returns 1 without running the network (the old firmware ran the
//     whole window);
//   - on a build without the ESP32-S3 SIMD assembly (host x86) dot() is plain C with the same integer
//     result, par() runs both halves in sequence, and heap_caps is malloc.
// Build with -fno-fast-math -ffp-contract=off (float FFT must match the verified build).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#define WH_USE_TASK 1
#else
#include "esp_heap_caps.h"  // host shim
#include "esp_log.h"        // host shim
#define WH_USE_TASK 0
#endif

#ifdef ESP_PLATFORM
#ifdef CONFIG_STT32_WAKEHUBERT_EQ
#define WH_EQ 1
#endif
#elif defined(STT32_WH_EQ)
#define WH_EQ 1   // host: -DSTT32_WH_EQ=1
#endif
#ifndef WH_EQ
#define WH_EQ 0
#endif
// Where the cut (trim) is made relative to the correction. 0 = correct the whole window, then trim (DLS-1074);
// 1 = trim the uncorrected window, correct only [seg_s, seg_e) ('same' mode, zero outside the segment), mel on that (DLS-1078/1079).
#ifdef ESP_PLATFORM
#ifdef CONFIG_STT32_WAKEHUBERT_EQ_TRIM_FIRST
#define WH_EQ_SEG 1
#endif
#elif defined(STT32_WH_EQ_SEG)
#define WH_EQ_SEG STT32_WH_EQ_SEG   // host: -DSTT32_WH_EQ_SEG=1
#endif
#ifndef WH_EQ_SEG
#define WH_EQ_SEG 0
#endif
// EQ implementation: 0 = direct float FIR, 1 = FFT overlap-save, 2 = int16 FIR (esp-dsp)
#ifdef ESP_PLATFORM
#if defined(CONFIG_STT32_WAKEHUBERT_EQ_IMPL_FFT)
#define WH_EQ_IMPL 1
#define WH_EQ_FFT_N CONFIG_STT32_WAKEHUBERT_EQ_FFT_N
#elif defined(CONFIG_STT32_WAKEHUBERT_EQ_IMPL_S16)
#define WH_EQ_IMPL 2
#endif
#elif defined(STT32_WH_EQ_IMPL)
#define WH_EQ_IMPL STT32_WH_EQ_IMPL   // host: -DSTT32_WH_EQ_IMPL=0|1|2 [-DWH_EQ_FFT_N=1024|2048]
#endif
#ifndef WH_EQ_IMPL
#define WH_EQ_IMPL 0
#endif

#include "frontend.h"
#include "qlayout.h"
#ifdef STT32_WH_NC
static_assert(STT32_WH_NC == STT32_WH_CLASSES, "generated/qlayout.h class count != STT32_WH_CLASSES (pass -DSTT32_WH_CLASSES=<n>)");
#endif
#include "stt32_wakehubert.h"

#if defined(ESP_PLATFORM) && defined(CONFIG_IDF_TARGET_ESP32S3)
#define WH_DOT_ASM 1
extern "C" int32_t word23_dot_s8_s3(const int8_t* a, const int8_t* b, uint32_t n, const int32_t* zero);
#else
#define WH_DOT_ASM 0
#endif

#ifdef STT32_WH_EMBED_WEIGHTS
extern "C" const uint8_t stt32_wakehubert_weights[];
extern "C" const uint8_t stt32_wakehubert_weights_end[];
#endif

static const char* TAG = "stt32_wh";

static const int NC = STT32_WH_CLASSES, NNET = STT32_WH_NETS, W = 64, fe_WIN = 320;
static const int PADR = 32, GTC = 25;
// Sizes for the largest window (48,000 samples): mel frames, encoder frames.
static const int NMAX = STT32_WINDOW_SAMPLES;
static const int TMAX = (NMAX + 240 - 400) / 160 + 1;
static const int FMAX = (TMAX + 2 - 4) / 2 + 1;
static const int CCH = 256;

// ---- small static state (the only static arrays) ----
static int8_t g_h[2][2][128] __attribute__((aligned(16)));
static int8_t g_ln[2][256] __attribute__((aligned(16)));
static int8_t g_f2[2][128] __attribute__((aligned(16)));

#if WH_DOT_ASM
static int32_t g_zero[4] __attribute__((aligned(16))) = {0, 0, 0, 0};
static inline int32_t dot(const int8_t* a, const int8_t* b, int n) { return word23_dot_s8_s3(a, b, (uint32_t)n, g_zero); }
#else
static inline int32_t dot(const int8_t* a, const int8_t* b, int n) {   // n: multiple of 16, as the asm needs
    int32_t s = 0;
    for (int i = 0; i < n; i++) s += (int32_t)a[i] * (int32_t)b[i];
    return s;
}
#endif

static const uint8_t* g_wb;   // weights base (16-byte aligned)
static bool g_split_rows;      // decided once in init(): GRU rows split over both cores (job_gru2) or one direction per core (job_gru)
static const uint8_t* g_gru;   // GRU weights, OFF_GRU_BEG .. OFF_GRU_END (flash or the work_caps copy)
// The 16 GRU tensors are copied as one block [BEG, END). Check qlayout.h keeps them inside it and that it ends at the next tensor.
constexpr size_t OFF_GRU_BEG = 351744, OFF_GRU_END = 966144;
static_assert(OFF_G_WIH[0][0] == OFF_GRU_BEG, "GRU block must start at the first GRU tensor");
static_assert(OFF_GRU_END == OFF_OUT_W, "GRU block must end where the next tensor (OFF_OUT_W) starts");
#define WH_GRU_IN_RANGE(a) static_assert((a)[0][0] >= OFF_GRU_BEG && (a)[0][0] < OFF_GRU_END && (a)[0][1] >= OFF_GRU_BEG && (a)[0][1] < OFF_GRU_END && (a)[1][0] >= OFF_GRU_BEG && (a)[1][0] < OFF_GRU_END && (a)[1][1] >= OFF_GRU_BEG && (a)[1][1] < OFF_GRU_END, #a " outside the GRU block")
WH_GRU_IN_RANGE(OFF_G_WIH); WH_GRU_IN_RANGE(OFF_G_WHH); WH_GRU_IN_RANGE(OFF_G_BIH);
WH_GRU_IN_RANGE(OFF_G_BHH); WH_GRU_IN_RANGE(OFF_G_MIH); WH_GRU_IN_RANGE(OFF_G_MHH);
static_assert(OFF_G_MHH[1][1] + 384 * sizeof(int32_t) == OFF_GRU_END, "the last GRU tensor must end at the block end");
static int64_t g_stage_us[10];      // per-stage time of the last infer(): trim mel stem blocks gru0 gru1 out cls eq (slot 8) (device only; read by replay)
extern "C" int stt32_wakehubert_stage_us(int64_t* out, int n) { for (int i = 0; i < n && i < 10; i++) out[i] = g_stage_us[i]; return 10; }
static inline const int8_t* G8(size_t off) { return (const int8_t*)(g_gru + (off - OFF_GRU_BEG)); }
static inline const int32_t* G32(size_t off) { return (const int32_t*)(g_gru + (off - OFF_GRU_BEG)); }
static inline const int8_t* P8(size_t off) { return (const int8_t*)(g_wb + off); }
static inline const int32_t* P32(size_t off) { return (const int32_t*)(g_wb + off); }
static inline const float* PF(size_t off) { return (const float*)(g_wb + off); }
static inline int32_t clampi(int64_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : (int32_t)v); }
static inline int64_t rq(int32_t acc, int32_t M, int S) { return ((int64_t)acc * M + ((int64_t)1 << (S - 1))) >> S; }
static const int32_t M33 = 33025;   // 2^22/127

// ---- allocations (init) ----
struct Mem {
    int8_t *mel8, *A, *B, *c1buf[2];
    int16_t* pre[2];
    int32_t* acc;   // [NNET][NC]
    float* lg;      // [NNET][NC]
    fe::Ws* ws;     // [2]
    void *tw, *win, *mw;
    double* trim;   // db[160], srt[160]
    int16_t* eq;    // WH_EQ: corrected window, lives in the A/B block (dead after the mel stage)
};
static Mem g_m;
struct AllocRec { const char* name; void* p; size_t bytes; };
static AllocRec g_rec[24];   // 14 base + 3 for the EQ implementations
static int g_nrec;
static bool g_inited;
#ifdef ESP_PLATFORM
static bool g_mapped;
static esp_partition_mmap_handle_t g_mh;
#endif

static void* walloc(const char* name, size_t n, uint32_t caps) {
    n = (n + 15) & ~(size_t)15;
    void* p = heap_caps_aligned_alloc(16, n, caps);
    if (!p) { ESP_LOGE(TAG, "alloc %s %u B failed (caps 0x%x)", name, (unsigned)n, (unsigned)caps); return nullptr; }
    memset(p, 0, n);
    g_rec[g_nrec++] = {name, p, n};
    return p;
}
static void free_all() {
    for (int i = 0; i < g_nrec; i++) heap_caps_free(g_rec[i].p);
    g_nrec = 0; memset(&g_m, 0, sizeof g_m);
#ifdef ESP_PLATFORM
    if (g_mapped) { esp_partition_munmap(g_mh); g_mapped = false; }
#endif
}
extern "C" int stt32_wakehubert_alloc_info(int i, const char** name, size_t* bytes) {
    if (i < 0 || i >= g_nrec) return 0;
    if (name) *name = g_rec[i].name;
    if (bytes) *bytes = g_rec[i].bytes;
    return 1;
}

// ---- 2 cores ----
typedef void (*job_fn)(int part, void* ctx);
#if WH_USE_TASK
static TaskHandle_t g_worker; static SemaphoreHandle_t g_done, g_exited; static job_fn g_job; static void* g_ctx;
static void worker_task(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!g_job) break;
        g_job(1, g_ctx); xSemaphoreGive(g_done);
    }
    xSemaphoreGive(g_exited);
    vTaskDelete(nullptr);
}
static void par(job_fn f, void* ctx) { g_job = f; g_ctx = ctx; xTaskNotifyGive(g_worker); f(0, ctx); xSemaphoreTake(g_done, portMAX_DELAY); }
#else
static void par(job_fn f, void* ctx) { f(0, ctx); f(1, ctx); }
#endif
static inline void split(int n, int part, int& a, int& b) { a = part ? n / 2 : 0; b = part ? n : n / 2; }

struct Run {
    int nsamp, T, F, C;
    const int16_t* seg; int8_t *mel8, *A, *B, *featbuf, *c1buf[2];
    int cur_blk, cur_layer; int16_t* pre[2];
    int32_t (*acc)[NC]; float (*lg)[NC];
};

static void job_mel(int part, void* c) {
    Run& r = *(Run*)c; int np = (r.T + 1) / 2; int a, b; split(np, part, a, b);
    float la[64], lb[64]; const float* ba = PF(OFF_BN_A); const float* bb = PF(OFF_BN_B);
    for (int p = a; p < b; p++) {
        int fa = 2 * p, fb = 2 * p + 1 < r.T ? 2 * p + 1 : r.T - 1;
        fe::mel_pair(g_m.ws[part], r.seg, r.nsamp, fa, fb, la, lb);
        int8_t* da = r.mel8 + (size_t)(2 + fa) * 64;
        for (int k = 0; k < 64; k++) da[k] = (int8_t)clampi((int64_t)lrintf(la[k] * ba[k] + bb[k]), -128, 127);
        if (2 * p + 1 < r.T) { int8_t* db = da + 64; for (int k = 0; k < 64; k++) db[k] = (int8_t)clampi((int64_t)lrintf(lb[k] * ba[k] + bb[k]), -128, 127); }
    }
}
static void job_stem(int part, void* c) {
    Run& r = *(Run*)c; int a, b; split(r.F, part, a, b); int C = r.C;
    int8_t* out = r.A + (size_t)PADR * C; const int8_t* w = P8(OFF_STEM_W); const int32_t *bi = P32(OFF_STEM_B), *M = P32(OFF_STEM_M);
    for (int k = 0; k < C; k++) { const int8_t* row = w + (size_t)k * 256;
        for (int f = a; f < b; f++) out[(size_t)f * C + k] = (int8_t)clampi(rq(dot(r.mel8 + (size_t)(2 * f) * 64, row, 256) + bi[k], M[k], S_STEM), 0, 127); }
}
static void job_dw(int part, void* c) {
    Run& r = *(Run*)c; int a, b; split(r.F, part, a, b); int C = r.C, i = r.cur_blk, d = 1 << (i % 4);
    const int8_t* w = P8(OFF_DW_W[i]); const int32_t *bi = P32(OFF_DW_B[i]), *M = P32(OFF_DW_M[i]); int S = S_DW[i];
    int8_t* x = r.A + (size_t)PADR * C; int8_t* tmp = r.B + (size_t)PADR * C;
    for (int t = a; t < b; t++) {
        const int8_t *x0 = x + (size_t)(t - 4 * d) * C, *x1 = x + (size_t)(t - 3 * d) * C, *x2 = x + (size_t)(t - 2 * d) * C, *x3 = x + (size_t)(t - d) * C, *x4 = x + (size_t)t * C;
        int8_t* o = tmp + (size_t)t * C;
        for (int k = 0; k < C; k++) {
            int32_t acc = w[k] * x0[k] + w[C + k] * x1[k] + w[2 * C + k] * x2[k] + w[3 * C + k] * x3[k] + w[4 * C + k] * x4[k] + bi[k];
            o[k] = (int8_t)clampi(rq(acc, M[k], S), 0, 127);
        }
    }
}
static void job_pw(int part, void* c) {
    Run& r = *(Run*)c; int a, b; split(r.F, part, a, b); int C = r.C, i = r.cur_blk;
    const int8_t* w = P8(OFF_PW_W[i]); const int32_t *bi = P32(OFF_PW_B[i]), *M = P32(OFF_PW_M[i]); int S = S_PW[i];
    int8_t* x = r.A + (size_t)PADR * C; const int8_t* tmp = r.B + (size_t)PADR * C;
    for (int k = 0; k < C; k++) { const int8_t* row = w + (size_t)k * C;
        for (int t = a; t < b; t++) { int64_t v = rq(dot(tmp + (size_t)t * C, row, C) + bi[k], M[k], S); x[(size_t)t * C + k] = (int8_t)clampi(v + x[(size_t)t * C + k], 0, 127); } }
}
static void job_gru(int part, void* c) {   // part = 向き（0 前向き, 1 後ろ向き）
    Run& r = *(Run*)c; int l = r.cur_layer, F = r.F;
    const int8_t* in = (l == 0 ? r.A : r.B) + (size_t)PADR * 256; int8_t* out = (l == 0 ? r.B : r.A) + (size_t)PADR * 256;
    const int8_t *wih = G8(OFF_G_WIH[l][part]), *whh = G8(OFF_G_WHH[l][part]);
    const int32_t *bih = G32(OFF_G_BIH[l][part]), *bhh = G32(OFF_G_BHH[l][part]), *Mih = G32(OFF_G_MIH[l][part]), *Mhh = G32(OFF_G_MHH[l][part]);
    const int Sih = S_G_IH[l][part], Shh = S_G_HH[l][part];
    int8_t *h = g_h[part][0], *h2 = g_h[part][1]; memset(h, 0, 128); memset(h2, 0, 128);
    int16_t* pre = r.pre[part];
    for (int c0 = 0; c0 < F; c0 += GTC) {
        int n = F - c0 < GTC ? F - c0 : GTC;
        for (int j = 0; j < 384; j++) { const int8_t* row = wih + (size_t)j * 256;
            for (int q = 0; q < n; q++) { int t = part ? F - 1 - (c0 + q) : c0 + q; pre[q * 384 + j] = (int16_t)clampi(rq(dot(in + (size_t)t * 256, row, 256) + bih[j], Mih[j], Sih), -2048, 2047); } }
        for (int q = 0; q < n; q++) {
            int t = part ? F - 1 - (c0 + q) : c0 + q; const int16_t* pq = pre + q * 384;
            for (int j = 0; j < 128; j++) {
                int32_t hr = clampi(rq(dot(h, whh + (size_t)j * 128, 128) + bhh[j], Mhh[j], Shh), -2048, 2047);
                int32_t hz = clampi(rq(dot(h, whh + (size_t)(128 + j) * 128, 128) + bhh[128 + j], Mhh[128 + j], Shh), -2048, 2047);
                int32_t hn = clampi(rq(dot(h, whh + (size_t)(256 + j) * 128, 128) + bhh[256 + j], Mhh[256 + j], Shh), -2048, 2047);
                int32_t rg = SIG[clampi(pq[j] + hr, -128, 127) + 128], zg = SIG[clampi(pq[128 + j] + hz, -128, 127) + 128];
                int32_t qn = (int32_t)((((int64_t)rg * hn) * M33 + ((int64_t)1 << 21)) >> 22);
                int32_t ng = TANHT[clampi(pq[256 + j] + qn, -128, 127) + 128];
                int32_t v = (127 - zg) * ng + zg * h[j];
                h2[j] = (int8_t)clampi((((int64_t)v) * M33 + ((int64_t)1 << 21)) >> 22, -127, 127);
            }
            memcpy(out + (size_t)t * 256 + part * 128, h2, 128);
            int8_t* tp = h; h = h2; h2 = tp;
        }
    }
}
#if WH_USE_TASK
// ---- 2-core barrier (internal DRAM flags, no RMW) ----
static volatile uint32_t g_bar[2] __attribute__((aligned(16)));
static inline void bar_wait(int part) {
    uint32_t my = g_bar[part] + 1;
    __atomic_store_n(&g_bar[part], my, __ATOMIC_RELEASE);
    while ((int32_t)(__atomic_load_n(&g_bar[1 - part], __ATOMIC_ACQUIRE) - my) < 0) { }
}
// Same arithmetic as job_gru, but one direction at a time and the 384 gate rows of every step split over the 2 cores.
static void job_gru2(int part, void* c) {
    Run& r = *(Run*)c; int l = r.cur_layer, F = r.F;
    const int8_t* inb = (l == 0 ? r.A : r.B) + (size_t)PADR * 256; int8_t* outb = (l == 0 ? r.B : r.A) + (size_t)PADR * 256;
    for (int dir = 0; dir < 2; dir++) {
        const int8_t *wih = G8(OFF_G_WIH[l][dir]), *whh = G8(OFF_G_WHH[l][dir]);
        const int32_t *bih = G32(OFF_G_BIH[l][dir]), *bhh = G32(OFF_G_BHH[l][dir]), *Mih = G32(OFF_G_MIH[l][dir]), *Mhh = G32(OFF_G_MHH[l][dir]);
        const int Sih = S_G_IH[l][dir], Shh = S_G_HH[l][dir];
        int8_t *h = g_h[0][0], *h2 = g_h[0][1];
        if (part == 0) { memset(h, 0, 128); memset(h2, 0, 128); }
        bar_wait(part);
        int16_t* pre = r.pre[0];
        const int ja = part * 64, jb = ja + 64;          // this core's hidden units
        for (int c0 = 0; c0 < F; c0 += GTC) {
            int n = F - c0 < GTC ? F - c0 : GTC;
            for (int j = part * 192; j < part * 192 + 192; j++) { const int8_t* row = wih + (size_t)j * 256;
                for (int q = 0; q < n; q++) { int t = dir ? F - 1 - (c0 + q) : c0 + q; pre[q * 384 + j] = (int16_t)clampi(rq(dot(inb + (size_t)t * 256, row, 256) + bih[j], Mih[j], Sih), -2048, 2047); } }
            bar_wait(part);
            for (int q = 0; q < n; q++) {
                int t = dir ? F - 1 - (c0 + q) : c0 + q; const int16_t* pq = pre + q * 384;
                for (int j = ja; j < jb; j++) {
                    int32_t hr = clampi(rq(dot(h, whh + (size_t)j * 128, 128) + bhh[j], Mhh[j], Shh), -2048, 2047);
                    int32_t hz = clampi(rq(dot(h, whh + (size_t)(128 + j) * 128, 128) + bhh[128 + j], Mhh[128 + j], Shh), -2048, 2047);
                    int32_t hn = clampi(rq(dot(h, whh + (size_t)(256 + j) * 128, 128) + bhh[256 + j], Mhh[256 + j], Shh), -2048, 2047);
                    int32_t rg = SIG[clampi(pq[j] + hr, -128, 127) + 128], zg = SIG[clampi(pq[128 + j] + hz, -128, 127) + 128];
                    int32_t qn = (int32_t)((((int64_t)rg * hn) * M33 + ((int64_t)1 << 21)) >> 22);
                    int32_t ng = TANHT[clampi(pq[256 + j] + qn, -128, 127) + 128];
                    int32_t v = (127 - zg) * ng + zg * h[j];
                    h2[j] = (int8_t)clampi((((int64_t)v) * M33 + ((int64_t)1 << 21)) >> 22, -127, 127);
                }
                memcpy(outb + (size_t)t * 256 + dir * 128 + ja, h2 + ja, 64);
                bar_wait(part);
                int8_t* tp = h; h = h2; h2 = tp;
            }
        }
    }
}
#else
static void job_gru2(int, void*) {}
#endif
static uint64_t isqrt64(uint64_t n) {   // floor(sqrt(n))（桁ごとの筆算。math.isqrt と同じ）
    uint64_t res = 0, bit = (uint64_t)1 << 62;
    while (bit > n) bit >>= 2;
    while (bit) { if (n >= res + bit) { n -= res + bit; res = (res >> 1) + bit; } else res >>= 1; bit >>= 2; }
    return res;
}
static void job_out(int part, void* c) {   // 整数 LayerNorm -> Linear 256->128（LN の affine と MU/SD は畳み込み済み）
    Run& r = *(Run*)c; int a, b; split(r.F, part, a, b); int C = r.C;
    const int8_t* in = r.A + (size_t)PADR * C; const int8_t* w = P8(OFF_OUT_W); const int32_t *bi = P32(OFF_OUT_B), *M = P32(OFF_OUT_M);
    for (int t = a; t < b; t++) {
        const int8_t* x = in + (size_t)t * C; int32_t S1 = 0, S2 = 0;
        for (int k = 0; k < C; k++) { S1 += x[k]; S2 += x[k] * x[k]; }
        int64_t V = 256LL * S2 - (int64_t)S1 * S1, D = V + 10570;
        int32_t sd = (int32_t)isqrt64((uint64_t)(4096 * D));
        for (int k = 0; k < C; k++) {
            int32_t num = SN * 64 * (256 * x[k] - S1), an = num < 0 ? -num : num;
            int32_t q = (2 * an + sd) / (2 * sd); if (num < 0) q = -q;
            g_ln[part][k] = (int8_t)clampi(q, -127, 127);
        }
        int8_t* o = r.featbuf + (size_t)(2 + t) * 128;
        for (int k = 0; k < 128; k++) o[k] = (int8_t)clampi(rq(dot(g_ln[part], w + (size_t)k * C, C) + bi[k], M[k], S_OUT), -127, 127);
    }
}
static void job_cls(int part, void* c) {
    Run& r = *(Run*)c; int F = r.F;
    int8_t* c1 = r.c1buf[part];
    for (int n = part * (NNET / 2); n < (part + 1) * (NNET / 2); n++) {
        memset(c1, 0, (size_t)(F + 4) * 64);
        const int8_t *w1 = P8(OFF_N_W1[n]), *w2 = P8(OFF_N_W2[n]), *fc = P8(OFF_N_FC[n]);
        const int32_t *b1 = P32(OFF_N_B1[n]), *M1 = P32(OFF_N_M1[n]), *b2 = P32(OFF_N_B2[n]), *M2 = P32(OFF_N_M2[n]), *bf = P32(OFF_N_BFC[n]);
        const float* L = PF(OFF_N_LFC[n]);
        int32_t sum[W]; int32_t mx[W]; for (int k = 0; k < W; k++) { sum[k] = 0; mx[k] = 0; }
        for (int k = 0; k < W; k++) { const int8_t* row = w1 + (size_t)k * 640;
            for (int t = 0; t < F; t++) c1[(size_t)(2 + t) * 64 + k] = (int8_t)clampi(rq(dot(r.featbuf + (size_t)t * 128, row, 640) + b1[k], M1[k], S_N1[n]), 0, 127); }
        for (int k = 0; k < W; k++) { const int8_t* row = w2 + (size_t)k * 320;
            for (int t = 0; t < F; t++) { int32_t v = clampi(rq(dot(c1 + (size_t)t * 64, row, 320) + b2[k], M2[k], S_N2[n]), 0, 127); sum[k] += v; if (v > mx[k]) mx[k] = v; } }
        for (int k = 0; k < W; k++) { g_f2[part][k] = (int8_t)((sum[k] * 2 + F) / (2 * F)); g_f2[part][W + k] = (int8_t)mx[k]; }
        for (int k = 0; k < NC; k++) { int32_t a = dot(g_f2[part], fc + (size_t)k * 128, 128) + bf[k]; r.acc[n][k] = a; r.lg[n][k] = (float)a * L[k]; }
    }
}
// ---- 無音の切り出し（extract_trim.py の bounds/trim と同じ式。int16 入力）----
static bool trim_bounds(const int16_t* p, int n, int& s, int& e) {
    int nf = n / fe_WIN; if (nf < 1) nf = 1;
    double* db = g_m.trim; double emax = 0;
    for (int i = 0; i < nf; i++) {
        int64_t ss = 0; for (int k = 0; k < fe_WIN; k++) { int32_t v = p[i * fe_WIN + k]; ss += (int64_t)v * v; }
        double m = ((double)ss * (1.0 / 1073741824.0)) / (double)fe_WIN;
        db[i] = sqrt(m + 1e-12); if (db[i] > emax) emax = db[i];
    }
    for (int i = 0; i < nf; i++) db[i] = 20.0 * log10(db[i] / (emax + 1e-12));
    double* srt = g_m.trim + 160; memcpy(srt, db, sizeof(double) * nf);
    for (int i = 1; i < nf; i++) { double v = srt[i]; int j = i - 1; while (j >= 0 && srt[j] > v) { srt[j + 1] = srt[j]; j--; } srt[j + 1] = v; }
    double vi = (double)(nf - 1) * 0.1; int pi = (int)floor(vi); double g = vi - (double)pi; double lo = srt[pi], hi = srt[pi + 1 < nf ? pi + 1 : pi];
    double diff = hi - lo; double pc = g >= 0.5 ? hi - diff * (1.0 - g) : lo + diff * g;
    double thr = pc + 12.0; if (thr < -45.0) thr = -45.0;
    int first = -1, last = -1;
    for (int i = 0; i < nf; i++) if (db[i] > thr) { if (first < 0) first = i; last = i; }
    if (first < 0) return false;
    int b0 = first * fe_WIN, b1 = (last + 1) * fe_WIN; if (b1 > n) b1 = n;
    s = b0 - 1440; if (s < 0) s = 0; e = b1 + 4960; if (e > n) e = n;
    return true;
}


#if WH_EQ
// ---- frequency-response correction (DLS-1069 S-A) ----
// y = clip(round(same-mode convolution of x with h*G)), h linear-phase and symmetric (511 taps, centre 255):
// y[i] = c0*x[i] + sum_{d=1..255} c[d]*(x[i-d] + x[i+d]), x = 0 outside the window. float accumulation, d ascending.
// Three implementations (WH_EQ_IMPL): 0 = direct float FIR (reference, always compiled), 1 = FFT overlap-save,
// 2 = int16 FIR (esp-dsp dsps_fird_s16, Q14 coefficients). The reference also serves the s16 fallback.
#include "eq_fir_coeffs.h"
static bool g_eq_on = true;
static inline int16_t eq_out(float a) { long q = lrintf(a); return (int16_t)(q < -32768 ? -32768 : (q > 32767 ? 32767 : q)); }
static void eq_fir_range(const int16_t* x, int16_t* y, int n, int i0, int i1) {
    const int H = (EQ_FIR_TAPS - 1) / 2;   // 255
    const float* c = k_eq_fir + H;         // c[d] = h[H + d]
    for (int i = i0; i < i1; i++) {
        float a = c[0] * (float)x[i];
        if (i >= H && i + H < n) {
            const int16_t* l = x + i; const int16_t* r = x + i;
            for (int d = 1; d <= H; d++) a += c[d] * (float)((int32_t)l[-d] + (int32_t)r[d]);
        } else {
            for (int d = 1; d <= H; d++) {
                int32_t s = (i - d >= 0 ? x[i - d] : 0) + (i + d < n ? x[i + d] : 0);
                a += c[d] * (float)s;
            }
        }
        y[i] = eq_out(a);
    }
}
static void eq_fir_apply_ref(const int16_t* x, int16_t* y, int n) { eq_fir_range(x, y, n, 0, n); }

#if WH_EQ_IMPL == 1
// FFT overlap-save, N-point complex FFT, two real blocks per FFT (block a in re, block b in im; H is real and even, so
// IFFT(FFT(a + jb) * H) = conv(a) + j conv(b)). The kernel is centred (taps at circular offsets -255..255), so the
// outputs p = 255 .. N-256 of every block are valid and are y[b*L + p - 255]. IFFT(X) = swap(FFT(swap(X))) / N.
#ifndef WH_EQ_FFT_N
#define WH_EQ_FFT_N 1024
#endif
static_assert(WH_EQ_FFT_N == 1024 || WH_EQ_FFT_N == 2048, "EQ FFT length: 1024 or 2048");
#ifdef ESP_PLATFORM
#include "dsps_fft2r.h"
static void eq_fft(float* d, int N) { dsps_fft2r_fc32(d, N); dsps_bit_rev2r_fc32(d, N); }   // natural order in and out
static bool eq_fft_setup(float* tw, int N) { return dsps_fft2r_init_fc32(tw, N) == ESP_OK; }
static void eq_fft_teardown() { dsps_fft2r_deinit_fc32(); }
#else
static double* g_hw;   // host: exp(-2 pi i k / N), k < N/2
static void eq_fft(float* d, int N) {
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit;
        if (i < j) { float t = d[2*i]; d[2*i] = d[2*j]; d[2*j] = t; t = d[2*i+1]; d[2*i+1] = d[2*j+1]; d[2*j+1] = t; }
    }
    for (int len = 2; len <= N; len <<= 1) {
        const int st = N / len;
        for (int i = 0; i < N; i += len)
            for (int k = 0; k < len / 2; k++) {
                const float wr = (float)g_hw[2*k*st], wi = (float)g_hw[2*k*st+1];
                float* u = d + 2*(i + k); float* v = d + 2*(i + k + len/2);
                const float tr = v[0]*wr - v[1]*wi, ti = v[0]*wi + v[1]*wr;
                v[0] = u[0] - tr; v[1] = u[1] - ti; u[0] += tr; u[1] += ti;
            }
    }
}
static bool eq_fft_setup(float*, int N) {
    g_hw = (double*)malloc(sizeof(double) * N);
    for (int k = 0; k < N/2; k++) { g_hw[2*k] = cos(-2.0*M_PI*k/N); g_hw[2*k+1] = sin(-2.0*M_PI*k/N); }
    return true;
}
static void eq_fft_teardown() { free(g_hw); g_hw = nullptr; }
#endif
static float* g_eq_buf;   // 2N floats (complex work block)
static float* g_eq_H;     // N/2+1 floats: real frequency response of the centred kernel
static float* g_eq_tw;    // twiddle table handed to the library (N floats)
static bool eq_impl_init(uint32_t cp) {
    const int N = WH_EQ_FFT_N, H = (EQ_FIR_TAPS - 1) / 2;
    g_eq_buf = (float*)walloc("eq_fft_buf", sizeof(float) * 2 * N, cp);
    g_eq_H = (float*)walloc("eq_fft_H", sizeof(float) * (N / 2 + 1), cp);
    g_eq_tw = (float*)walloc("eq_fft_tw", sizeof(float) * N, cp);
    if (!g_eq_buf || !g_eq_H || !g_eq_tw) return false;
    if (!eq_fft_setup(g_eq_tw, N)) return false;
    g_eq_buf[0] = k_eq_fir[H];
    for (int d = 1; d <= H; d++) { g_eq_buf[2*d] = k_eq_fir[H + d]; g_eq_buf[2*(N - d)] = k_eq_fir[H + d]; }
    eq_fft(g_eq_buf, N);
    for (int k = 0; k <= N / 2; k++) g_eq_H[k] = g_eq_buf[2*k];
    return true;
}
static void eq_impl_deinit() { eq_fft_teardown(); g_eq_buf = g_eq_H = g_eq_tw = nullptr; }
static void eq_fft_apply(const int16_t* x, int16_t* y, int n) {
    const int N = WH_EQ_FFT_N, H = (EQ_FIR_TAPS - 1) / 2, L = N - 2 * H;
    const int nb = (n + L - 1) / L;
    float* d = g_eq_buf; const float sc = 1.0f / (float)N;
    for (int b = 0; b < nb; b += 2) {
        const int s0 = b * L - H, s1 = (b + 1) * L - H;
        for (int p = 0; p < N; p++) {
            const int i0 = s0 + p, i1 = s1 + p;
            d[2*p] = (i0 >= 0 && i0 < n) ? (float)x[i0] : 0.0f;
            d[2*p+1] = (b + 1 < nb && i1 >= 0 && i1 < n) ? (float)x[i1] : 0.0f;
        }
        eq_fft(d, N);
        for (int k = 0; k < N; k++) {
            const float m = g_eq_H[k <= N/2 ? k : N - k];
            const float re = d[2*k], im = d[2*k+1];
            d[2*k] = im * m; d[2*k+1] = re * m;
        }
        eq_fft(d, N);
        for (int p = H; p < N - H; p++) {
            const int i0 = b * L + (p - H), i1 = (b + 1) * L + (p - H);
            if (i0 < n) y[i0] = eq_out(d[2*p+1] * sc);
            if (b + 1 < nb && i1 < n) y[i1] = eq_out(d[2*p] * sc);
        }
    }
}
static void eq_fir_apply(const int16_t* x, int16_t* y, int n) { eq_fft_apply(x, y, n); }
#elif WH_EQ_IMPL == 2
// int16 FIR. Coefficients h (|h| <= 1.298) are stored Q14 (round(h * 16384), max 21266), one extra zero tap = 512 (a
// multiple of 8, as the aes3 kernel wants); fir.shift = 1 so that out = (acc + 0x3fff) >> 14 (dsps_fird_s16: final shift
// 15 - shift). The library output is the low 16 bits of the shifted sum, it does not saturate, so every 256-sample
// chunk whose input halo could overflow (peak * sum|h| > 32766) is recomputed by the float reference.
#ifdef ESP_PLATFORM
#include "dsps_fir.h"
#endif
#define EQ_S16_TAPS 512
#define EQ_S16_CH 256
static int16_t* g_eq_cq;      // [512] coefficients, 16-aligned
static int16_t* g_eq_dl;      // [512] delay line, 16-aligned
static int32_t g_eq_peak_T;   // chunk is safe when its halo peak <= T
static long g_eq_fallback_chunks, g_eq_chunks;   // statistics of the last apply (test/diagnostic)
#ifdef ESP_PLATFORM
static fir_s16_t g_eq_fir;
#endif
static bool eq_impl_init(uint32_t cp) {
    g_eq_cq = (int16_t*)walloc("eq_s16_coef", EQ_S16_TAPS * 2, cp);
    g_eq_dl = (int16_t*)walloc("eq_s16_delay", EQ_S16_TAPS * 2, cp);
    if (!g_eq_cq || !g_eq_dl) return false;
    double sabs = 0;
    for (int k = 0; k < EQ_FIR_TAPS; k++) { g_eq_cq[k] = (int16_t)lrintf(k_eq_fir[k] * 16384.0f); sabs += fabs((double)g_eq_cq[k]); }
    g_eq_peak_T = (int32_t)floor(32766.0 / (sabs / 16384.0));
#ifdef ESP_PLATFORM
#if dsps_fird_s16_aes3_enabled
    dsps_16_array_rev(g_eq_cq, EQ_S16_TAPS);   // the aes3 kernel wants the coefficients reversed
#endif
    if (dsps_fird_init_s16(&g_eq_fir, g_eq_cq, g_eq_dl, EQ_S16_TAPS, 1, 0, 1) != ESP_OK) return false;
#endif
    return true;
}
static void eq_impl_deinit() {
#ifdef ESP_PLATFORM
    dsps_fird_s16_aexx_free(&g_eq_fir);
#endif
    g_eq_cq = g_eq_dl = nullptr;
}
static void eq_s16_apply(const int16_t* x, int16_t* y, int n) {
    const int H = (EQ_FIR_TAPS - 1) / 2;
    if (n < 2 * EQ_S16_TAPS) { g_eq_chunks = g_eq_fallback_chunks = 0; eq_fir_range(x, y, n, 0, n); return; }   // too short for the delay priming
#ifdef ESP_PLATFORM
    // dsps_fird_s16_aes3 does not store the delay position back into fir->pos, so a call cannot continue a previous one:
    // every call starts with pos = 0 and a delay line primed with the last 512 inputs (oldest first).
    static const int16_t zeros[256] __attribute__((aligned(16))) = {0};
    memset(g_eq_dl, 0, EQ_S16_TAPS * 2); memcpy(g_eq_dl + (EQ_S16_TAPS - H), x, H * 2);   // x[0..H-1] are the newest H
    g_eq_fir.pos = 0; g_eq_fir.d_pos = 0;
    dsps_fird_s16(&g_eq_fir, x + H, y, n - H);                       // z[H..n-1] = y[0..n-H-1]
    memcpy(g_eq_dl, x + (n - EQ_S16_TAPS), EQ_S16_TAPS * 2);          // x[n-512..n-1]
    g_eq_fir.pos = 0; g_eq_fir.d_pos = 0;
    dsps_fird_s16(&g_eq_fir, zeros, y + (n - H), H);                 // x = 0 beyond the window: y[n-H..n-1]
#else
    for (int i = 0; i < n; i++) {   // host model of dsps_fird_s16_ansi: acc = round + sum, out = low 16 bits of acc >> 14
        long long acc = 0x3fff;
        for (int k = 0; k < EQ_FIR_TAPS; k++) { const int j = i + H - k; if (j >= 0 && j < n) acc += (long long)g_eq_cq[k] * x[j]; }
        y[i] = (int16_t)(acc >> 14);
    }
#endif
    const int nch = (n + EQ_S16_CH - 1) / EQ_S16_CH;
    uint16_t pk[48000 / EQ_S16_CH + 2];
    for (int c = 0; c < nch; c++) {
        int m = 0; const int e = (c + 1) * EQ_S16_CH < n ? (c + 1) * EQ_S16_CH : n;
        for (int i = c * EQ_S16_CH; i < e; i++) { int v = x[i] < 0 ? -(int)x[i] : x[i]; if (v > m) m = v; }
        pk[c] = (uint16_t)(m > 65535 ? 65535 : m);
    }
    g_eq_chunks = nch; g_eq_fallback_chunks = 0;
    for (int c = 0; c < nch; c++) {
        int m = pk[c]; if (c > 0 && pk[c-1] > m) m = pk[c-1]; if (c + 1 < nch && pk[c+1] > m) m = pk[c+1];
        if (m > g_eq_peak_T) {
            g_eq_fallback_chunks++;
            const int e = (c + 1) * EQ_S16_CH < n ? (c + 1) * EQ_S16_CH : n;
            eq_fir_range(x, y, n, c * EQ_S16_CH, e);
        }
    }
}
static void eq_fir_apply(const int16_t* x, int16_t* y, int n) { eq_s16_apply(x, y, n); }
extern "C" void stt32_wakehubert_eq_stats(long* chunks, long* fallback) { *chunks = g_eq_chunks; *fallback = g_eq_fallback_chunks; }
#else
static bool eq_impl_init(uint32_t) { return true; }
static void eq_impl_deinit() {}
static void eq_fir_apply(const int16_t* x, int16_t* y, int n) { eq_fir_apply_ref(x, y, n); }
#endif
extern "C" int stt32_wakehubert_set_eq(int on) { g_eq_on = on != 0; return 0; }
extern "C" void stt32_wakehubert_eq_apply(const int16_t* in, int16_t* out, int n) { eq_fir_apply(in, out, n); }   // for tests: the selected implementation
// test hook: cut [s, e) on the uncorrected window, correct only the cut (zero outside), selected implementation or the float FIR
extern "C" int stt32_wakehubert_eq_seg(const int16_t* in, int n, int16_t* out, int use_ref, int* s, int* e) {
    if (!trim_bounds(in, n, *s, *e)) return 1;
    if (use_ref) eq_fir_apply_ref(in + *s, out, *e - *s); else eq_fir_apply(in + *s, out, *e - *s);
    return 0;
}
extern "C" void stt32_wakehubert_eq_apply_ref(const int16_t* in, int16_t* out, int n) { eq_fir_apply_ref(in, out, n); }   // for tests: the float FIR
#else
extern "C" int stt32_wakehubert_set_eq(int on) { return on ? -1 : 0; }   // not compiled in: only "off" is possible
#endif

// ---- init / deinit ----
// failure after the EQ was set up: release the esp-dsp FFT table state (initialized flag, table pointer) first, then the buffers.
// eq_impl_deinit() is idempotent (dsps_fft2r_deinit_fc32 clears its flags; the host free(NULL) is a no-op), and stt32_wakehubert_deinit()
// is not reached on a failed init (g_inited stays false), so nothing is released twice.
static void init_fail() {
#if WH_EQ
    eq_impl_deinit();
#endif
    free_all();
}
extern "C" esp_err_t stt32_wakehubert_init(const stt32_backend_config_t* cfg) {
    if (g_inited) return ESP_ERR_INVALID_STATE;
    if (!cfg) return ESP_ERR_INVALID_ARG;
    g_nrec = 0; memset(&g_m, 0, sizeof g_m);
    // weights
    const size_t need = OFF_N_LFC[NNET - 1] + (size_t)NC * sizeof(float);
#ifdef ESP_PLATFORM
    if (cfg->weights_partition) {
        const esp_partition_t* part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, cfg->weights_partition);
        if (!part) { ESP_LOGE(TAG, "partition %s not found", cfg->weights_partition); return ESP_ERR_NOT_FOUND; }
        // image = 16-byte header {"WKH1", u32 blob size, 8 reserved} + blob (tools/regen_wakehubert_weights.py --pack)
        const void* map = nullptr; esp_partition_mmap_handle_t h;
        esp_err_t e = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &map, &h);
        if (e != ESP_OK) { ESP_LOGE(TAG, "mmap %s: %s", cfg->weights_partition, esp_err_to_name(e)); return e; }
        const uint8_t* b = (const uint8_t*)map; uint32_t sz; memcpy(&sz, b + 4, 4);
        if (memcmp(b, "WKH1", 4) != 0 || sz < need || (size_t)sz + 16 > part->size) {
            ESP_LOGE(TAG, "partition %s: bad header (size %u, need >= %u)", cfg->weights_partition, (unsigned)sz, (unsigned)need);
            esp_partition_munmap(h); return ESP_ERR_INVALID_SIZE;
        }
        g_wb = b + 16; g_mh = h; g_mapped = true;
    } else
#endif
    {
#ifdef STT32_WH_EMBED_WEIGHTS
        if ((size_t)(stt32_wakehubert_weights_end - stt32_wakehubert_weights) < need) return ESP_ERR_INVALID_SIZE;
        g_wb = stt32_wakehubert_weights;
#else
        return ESP_ERR_INVALID_ARG;   // built without embedded weights: a partition is required
#endif
    }
    if (((uintptr_t)g_wb & 15) != 0) return ESP_ERR_INVALID_STATE;
    // work buffers and tables, sized for the largest (48,000-sample) window
    const uint32_t cp = cfg->work_caps;
    g_m.mel8 = (int8_t*)walloc("mel8", (size_t)(TMAX + 2) * 64 + 64, cp);
#if WH_EQ
    {   // A and B as one block, big enough for the corrected window (NMAX int16); the window is dead before the stem writes A
        const size_t ab = (((size_t)(PADR + FMAX) * CCH) + 15) & ~(size_t)15;
        size_t tot = ab * 2; if (tot < (size_t)NMAX * sizeof(int16_t)) tot = (size_t)NMAX * sizeof(int16_t);
        int8_t* blk = (int8_t*)walloc("AB_eq", tot, cp);
        g_m.A = blk; g_m.B = blk ? blk + ab : nullptr; g_m.eq = (int16_t*)blk;
    }
#else
    g_m.A = (int8_t*)walloc("A", (size_t)(PADR + FMAX) * CCH, cp);
    g_m.B = (int8_t*)walloc("B", (size_t)(PADR + FMAX) * CCH, cp);
#endif
    g_m.c1buf[0] = (int8_t*)walloc("c1buf0", (size_t)(FMAX + 4) * 64, cp);
    g_m.c1buf[1] = (int8_t*)walloc("c1buf1", (size_t)(FMAX + 4) * 64, cp);
    g_m.pre[0] = (int16_t*)walloc("pre0", (size_t)GTC * 384 * 2, cp);
    g_m.pre[1] = (int16_t*)walloc("pre1", (size_t)GTC * 384 * 2, cp);
    g_m.acc = (int32_t*)walloc("acc", (size_t)NNET * NC * 4, cp);
    g_m.lg = (float*)walloc("lg", (size_t)NNET * NC * 4, cp);
    g_m.ws = (fe::Ws*)walloc("fft_ws", sizeof(fe::Ws) * 2, cp);
    g_m.tw = walloc("fft_tw", fe::TW_BYTES, cp);
    g_m.win = walloc("hann_win", fe::WIN_BYTES, cp);
    g_m.mw = walloc("mel_weights", fe::MW_BYTES, cp);
    g_m.trim = (double*)walloc("trim_db", sizeof(double) * 320, cp);
    if (!g_m.mel8 || !g_m.A || !g_m.B || !g_m.c1buf[0] || !g_m.c1buf[1] || !g_m.pre[0] || !g_m.pre[1] || !g_m.acc || !g_m.lg ||
        !g_m.ws || !g_m.tw || !g_m.win || !g_m.mw || !g_m.trim) { free_all(); return ESP_ERR_NO_MEM; }
    fe::init(g_m.tw, g_m.win, g_m.mw);
#if WH_EQ
    if (!eq_impl_init(cp)) { init_fail(); return ESP_ERR_NO_MEM; }
#endif
    // Weight copies: the flash cache thrashes on the GRU's repeated reads (W_hh every step).
    g_gru = g_wb + OFF_GRU_BEG;
    if (cfg->weights_copy == STT32_WEIGHTS_COPY_ALL) {
        uint8_t* blk = (uint8_t*)walloc("weights_copy", need, cp);
        if (!blk) { init_fail(); return ESP_ERR_NO_MEM; }
        memcpy(blk, g_wb, need); g_wb = blk; g_gru = g_wb + OFF_GRU_BEG;
    } else if (cfg->weights_copy != STT32_WEIGHTS_COPY_NONE) {   // GRU (and DEFAULT)
        uint8_t* blk = (uint8_t*)walloc("gru_weights", OFF_GRU_END - OFF_GRU_BEG, cp);
        if (!blk) { init_fail(); return ESP_ERR_NO_MEM; }
        memcpy(blk, g_gru, OFF_GRU_END - OFF_GRU_BEG); g_gru = blk;
    }
#if WH_USE_TASK
    {   // cfg->infer_core outside the core range = not pinned; the worker is always pinned
        const bool infer_pinned = cfg->infer_core >= 0 && cfg->infer_core < (int)configNUMBER_OF_CORES;
        g_split_rows = infer_pinned && cfg->infer_core != cfg->worker_core;
    }
#else
    g_split_rows = false;
#endif
#if WH_USE_TASK
    g_done = xSemaphoreCreateBinary(); g_exited = xSemaphoreCreateBinary(); g_job = nullptr;
    if (!g_done || !g_exited) { if (g_done) vSemaphoreDelete(g_done); if (g_exited) vSemaphoreDelete(g_exited); g_done = g_exited = nullptr; init_fail(); return ESP_ERR_NO_MEM; }
    if (xTaskCreatePinnedToCore(worker_task, "stt32_wh_w1", 8192, nullptr, cfg->worker_priority, &g_worker, cfg->worker_core) != pdPASS) {
        vSemaphoreDelete(g_done); vSemaphoreDelete(g_exited); g_done = g_exited = nullptr; init_fail(); return ESP_ERR_NO_MEM;
    }
#endif
    size_t total = 0; for (int i = 0; i < g_nrec; i++) total += g_rec[i].bytes;
    ESP_LOGI(TAG, "GRU: %s (infer core %d, worker core %d)", g_split_rows ? "rows split over 2 cores (job_gru2)" : "one direction per half (job_gru)", cfg->infer_core, cfg->worker_core);
    ESP_LOGI(TAG, "init: %d allocations, %u B (caps 0x%x), weights %p", g_nrec, (unsigned)total, (unsigned)cp, (const void*)g_wb);
    g_inited = true;
    return ESP_OK;
}

extern "C" void stt32_wakehubert_deinit(void) {
    if (!g_inited) return;
#if WH_USE_TASK
    g_job = nullptr; xTaskNotifyGive(g_worker);
    xSemaphoreTake(g_exited, portMAX_DELAY);
    vSemaphoreDelete(g_done); vSemaphoreDelete(g_exited); g_done = g_exited = nullptr; g_worker = nullptr;
#endif
#if WH_EQ
    eq_impl_deinit();
#endif
    free_all();
    g_inited = false;
}

static uint32_t fnv(const uint8_t* p, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h; }

// ---- one window: 0 = ok, 1 = no speech (scores untouched), 2 = bad argument, 3 = not initialized ----
extern "C" int stt32_wakehubert_infer(const int16_t* pcm, size_t n_, float* scores, size_t class_count, stt32_wakehubert_info_t* info) {
    if (!g_inited) return 3;
    if (!pcm || !scores || class_count != (size_t)NC || n_ < (size_t)fe_WIN || n_ > (size_t)NMAX) return 2;
    int n = (int)n_;
    Run r; memset(&r, 0, sizeof r); r.C = CCH;
    int seg_s = 0, seg_e = 0;
#if WH_USE_TASK
#define TMARK(i) do { int64_t _n = esp_timer_get_time(); g_stage_us[i] = _n - _tm; _tm = _n; } while (0)
    int64_t _tm = esp_timer_get_time(); memset(g_stage_us, 0, sizeof g_stage_us);
#else
#define TMARK(i) do { } while (0)
#endif
#if WH_EQ && !WH_EQ_SEG
    if (g_eq_on) { eq_fir_apply(pcm, g_m.eq, n); pcm = g_m.eq; }
    TMARK(8);   // stage slot 8 = eq (0 when off); replay prints slots 0..7 only
#endif
    if (!trim_bounds(pcm, n, seg_s, seg_e)) return 1;
    TMARK(0);
    r.seg = pcm + seg_s; r.nsamp = seg_e - seg_s; r.T = (r.nsamp + 240 - 400) / 160 + 1;
    r.F = (r.T + 2 - 4) / 2 + 1;
    if (r.T < 2 || r.T > TMAX || r.F < 1 || r.F > FMAX) return 2;
#if WH_EQ && WH_EQ_SEG
    if (g_eq_on) { eq_fir_apply(r.seg, g_m.eq, r.nsamp); r.seg = g_m.eq; }   // only the cut, zero outside it
    TMARK(8);
#endif
    r.mel8 = g_m.mel8; r.A = g_m.A; r.B = g_m.B; r.c1buf[0] = g_m.c1buf[0]; r.c1buf[1] = g_m.c1buf[1];
    r.pre[0] = g_m.pre[0]; r.pre[1] = g_m.pre[1];
    r.acc = (int32_t(*)[NC])g_m.acc; r.lg = (float(*)[NC])g_m.lg;
    memset(r.mel8, 0, 2 * 64);
    par(job_mel, &r); TMARK(1);
    memset(r.A, 0, (size_t)PADR * r.C);
    par(job_stem, &r); TMARK(2);
    uint32_t mcrc = fnv((const uint8_t*)r.mel8 + 128, (size_t)r.T * 64);
    memset(r.B, 0, (size_t)PADR * r.C);
    r.featbuf = r.B;
    for (int i = 0; i < 4; i++) { r.cur_blk = i; par(job_dw, &r); par(job_pw, &r); }
    TMARK(3);
    // job_gru2 (one direction at a time, rows split over both cores) spins on a barrier, so it is only safe when
    // the two halves really run in parallel on different cores (decided in init). Otherwise (host, same core,
    // unpinned task) job_gru runs one direction per half.
    job_fn gj = g_split_rows ? job_gru2 : job_gru;
    r.cur_layer = 0; par(gj, &r); TMARK(4); r.cur_layer = 1; par(gj, &r); TMARK(5);
    memset(r.featbuf, 0, 2 * 128); memset(r.featbuf + (size_t)(2 + r.F) * 128, 0, 2 * 128);
    par(job_out, &r); TMARK(6);
    par(job_cls, &r); TMARK(7);
    uint32_t fcrc = fnv((const uint8_t*)r.featbuf + 256, (size_t)r.F * 128);
    // mean of the 10 softmax vectors
    float mp[NC]; for (int k = 0; k < NC; k++) mp[k] = 0;
    for (int nn = 0; nn < NNET; nn++) {
        float mxl = r.lg[nn][0]; for (int k = 1; k < NC; k++) if (r.lg[nn][k] > mxl) mxl = r.lg[nn][k];
        float e[NC], s = 0; for (int k = 0; k < NC; k++) { e[k] = expf(r.lg[nn][k] - mxl); s += e[k]; }
        for (int k = 0; k < NC; k++) mp[k] += e[k] / s;
    }
    for (int k = 0; k < NC; k++) scores[k] = mp[k] * (1.0f / NNET);
    if (info) {
        info->seg_s = seg_s; info->seg_e = seg_e; info->T = r.T; info->F = r.F; info->mel_crc = mcrc; info->feat_crc = fcrc;
        memcpy(info->acc, r.acc, sizeof info->acc);
    }
    return 0;
}
