// WakeStudent の CausalLogMel と同じ計算（n_fft 400, hop 160, hann, mel 64, log(x+1e-6)）。
// 400 点の複素 FFT（基数 4,4,5,5）で 2 フレームを実部・虚部に詰めて同時に変換する。
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
namespace fe {
struct cpx { float re, im; };
constexpr int NFFT = 400, HOP = 160, NF = 201, NMEL = 64, MAXW = 48;
// Tables live in memory the caller allocates (init(): work_caps, normally PSRAM).
// Only the 512 B of mel band bounds stay static.
static cpx* g_tw;                 // [NFFT]
static float* g_win;              // [NFFT]
static float (*g_mw)[MAXW];       // [NMEL][MAXW]
static int g_mstart[NMEL], g_mcnt[NMEL];
static const int FAC[4] = {4, 4, 5, 5};
constexpr size_t TW_BYTES = sizeof(cpx) * NFFT, WIN_BYTES = sizeof(float) * NFFT, MW_BYTES = sizeof(float) * NMEL * MAXW;
inline void init(void* tw, void* win, void* mw) {
    g_tw = (cpx*)tw; g_win = (float*)win; g_mw = (float (*)[MAXW])mw;
    memset(g_mw, 0, MW_BYTES);
    for (int k = 0; k < NFFT; k++) { g_tw[k].re = cosf(-2.0f * (float)M_PI * k / NFFT); g_tw[k].im = sinf(-2.0f * (float)M_PI * k / NFFT); }
    for (int i = 0; i < NFFT; i++) g_win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / NFFT);
    auto to_mel = [](float f) { return 2595.0f * log10f(1.0f + f / 700.0f); };
    float pts[NMEL + 2], fp[NMEL + 2];
    for (int i = 0; i < NMEL + 2; i++) { pts[i] = to_mel(0) + (to_mel(8000) - to_mel(0)) * i / (NMEL + 1); fp[i] = 700.0f * (powf(10.0f, pts[i] / 2595.0f) - 1.0f); }
    for (int m = 0; m < NMEL; m++) {
        int s = -1, c = 0;
        for (int k = 0; k < NF; k++) {
            float f = 8000.0f * k / (NF - 1);
            float down = -(fp[m] - f) / (fp[m + 1] - fp[m]);
            float up = (fp[m + 2] - f) / (fp[m + 2] - fp[m + 1]);
            float v = fminf(down, up); if (v < 0) v = 0;
            if (v > 0) { if (s < 0) s = k; int idx = k - s; if (idx < MAXW) g_mw[m][idx] = v; c = idx + 1; }
        }
        g_mstart[m] = s < 0 ? 0 : s; g_mcnt[m] = c > MAXW ? MAXW : c;
    }
}
inline void fftrec(const cpx* in, cpx* out, int n, int stride, int fi) {
    if (n == 1) { out[0] = *in; return; }
    int p = FAC[fi], m = n / p, step = NFFT / n;
    for (int r = 0; r < p; r++) fftrec(in + r * stride, out + r * m, m, stride * p, fi + 1);
    cpx t[5];
    for (int k = 0; k < m; k++) {
        for (int r = 0; r < p; r++) {
            cpx v = out[r * m + k], w = g_tw[(r * k * step) % NFFT];
            t[r].re = v.re * w.re - v.im * w.im; t[r].im = v.re * w.im + v.im * w.re;
        }
        for (int q = 0; q < p; q++) {
            float sr = 0, si = 0;
            for (int r = 0; r < p; r++) { cpx w = g_tw[(r * q * (NFFT / p)) % NFFT]; sr += t[r].re * w.re - t[r].im * w.im; si += t[r].re * w.im + t[r].im * w.re; }
            out[k + q * m].re = sr; out[k + q * m].im = si;
        }
    }
}
struct Ws { cpx in[NFFT], out[NFFT]; float pa[NF], pb[NF]; };
// フレーム f は「240 個の 0 + pcm」の [f*160, f*160+400)。
inline void get_frame(const int16_t* pcm, int n, int f, float* dst) {
    for (int i = 0; i < NFFT; i++) { int s = f * HOP + i - (NFFT - HOP); dst[i] = (s >= 0 && s < n) ? pcm[s] * (1.0f / 32768.0f) : 0.0f; }
}
// 2 フレーム (fa, fb) の log-mel（BN 前）を la[64], lb[64] に出す。
inline void mel_pair(Ws& w, const int16_t* pcm, int n, int fa, int fb, float* la, float* lb) {
    float a[NFFT], b[NFFT];
    get_frame(pcm, n, fa, a); get_frame(pcm, n, fb, b);
    for (int i = 0; i < NFFT; i++) { w.in[i].re = a[i] * g_win[i]; w.in[i].im = b[i] * g_win[i]; }
    fftrec(w.in, w.out, NFFT, 1, 0);
    for (int k = 0; k < NF; k++) {
        cpx z = w.out[k], zc = w.out[(NFFT - k) % NFFT]; zc.im = -zc.im;
        float sr = z.re + zc.re, si = z.im + zc.im, dr = z.re - zc.re, di = z.im - zc.im;
        w.pa[k] = 0.25f * (sr * sr + si * si); w.pb[k] = 0.25f * (dr * dr + di * di);
    }
    for (int m = 0; m < NMEL; m++) {
        float sa = 0, sb = 0; const float* mw = g_mw[m]; int s = g_mstart[m];
        for (int j = 0; j < g_mcnt[m]; j++) { sa += mw[j] * w.pa[s + j]; sb += mw[j] * w.pb[s + j]; }
        la[m] = logf(sa + 1e-6f); lb[m] = logf(sb + 1e-6f);
    }
}
}
