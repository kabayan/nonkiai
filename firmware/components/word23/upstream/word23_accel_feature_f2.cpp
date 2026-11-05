// Independent F2 candidate: original double arithmetic order, signed-zero k0
// specialization and sparse precomputed mel weights. Old source stays readonly.
#include "word23_accel_feature_f2.h"
#include "word23_accel_feature_f2_internal.h"
#include <cassert>
#include <cstdint>
#include <limits>
#include <new>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>


/*
 * Reference: training/train_fp32.py L54-113.
 *
 * Internal math is double. The Python reference runs float32 (scipy upcasts the
 * float32 STFT to complex64), but a portable radix-2 FFT in float32 diverges
 * MORE from scipy's float32 pocketfft than double does (measured: float32 raised
 * the frames max-abs-diff and broke a downstream match), because double stays
 * closest to the true value. The only residual discrepancy is at the EPS=1e-8
 * clamp: mel-power is scaled by (1/sum(hann))^2 ~ 2.5e-5, so silent bins land on
 * the clamp threshold, where a single frame can flip between log(EPS) and just
 * above it. That produces a ~0.69/std normalized swing on isolated frames of
 * signal-free bins; it does not move the classification (downstream stays 100%).
 * The G6a checker gates on a small frame-outlier rate plus 100% downstream to
 * cover this documented noise-floor artifact. FFT is a self-contained radix-2
 * (512 = 2^9): identical code on host and device, no external dependency.
 */

namespace {

constexpr int kFrameLength = STS_FEAT_FRAME_LENGTH; /* 400 */
constexpr int kFrameShift = STS_FEAT_FRAME_SHIFT;   /* 160 */
constexpr int kNfft = STS_FEAT_NFFT;                /* 512 */
constexpr int kMelBins = STS_FEAT_MEL_BINS;         /* 40 */
constexpr int kFeatureFrames = STS_FEAT_FRAMES;     /* 64 */
constexpr int kSpectrumBins = kNfft / 2 + 1;        /* 257 */
constexpr int kSampleRate = STS_FEAT_SAMPLE_RATE;
constexpr double kEps = 1.0e-8;      /* np.maximum(mel_power, EPS) */
constexpr double kNormEps = 1.0e-6;  /* std + 1e-6 in per-bin normalization */
constexpr double kPi = 3.14159265358979323846;

double hz_to_mel(double f) { return 2595.0 * std::log10(1.0 + f / 700.0); }
double mel_to_hz(double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); }

/* Periodic Hann window == scipy get_window('hann', 400, fftbins=True).
 * scipy stft scaling='spectrum', mode='stft' -> per-bin scale = 1 / win.sum(). */
struct Window {
    double w[kFrameLength];
    double scale;
    Window() {
        double sum = 0.0;
        for (int n = 0; n < kFrameLength; ++n) {
            w[n] = 0.5 - 0.5 * std::cos(2.0 * kPi * n / kFrameLength);
            sum += w[n];
        }
        scale = 1.0 / sum;
    }
};

/* Mel filterbank (40 x 257) == train_fp32.mel_filterbank(16000, 512, 40). */
struct MelFilterbank {
    int edges[kMelBins + 2];
    static constexpr size_t kSparseCapacity=2*kSpectrumBins;
    double sparse_weight[kSparseCapacity];
    uint16_t sparse_index[kSparseCapacity];
    uint16_t offsets[kMelBins+1];
    MelFilterbank() {
        const double mlo = hz_to_mel(80.0);
        const double mhi = hz_to_mel(kSampleRate / 2.0);
        for (int k = 0; k < kMelBins + 2; ++k) {
            double mel = mlo + (mhi - mlo) * static_cast<double>(k) / static_cast<double>(kMelBins + 2 - 1);
            double hz = mel_to_hz(mel);
            edges[k] = static_cast<int>(std::floor((kNfft + 1) * hz / kSampleRate));
        }
        size_t count=0;
        for(int m=0;m<kMelBins;++m) {
            offsets[m]=static_cast<uint16_t>(count);
            const int right=std::min(edges[m+2],kSpectrumBins);
            for(int k=std::max(0,edges[m]);k<right;++k) {
                const double w=weight(m,k); // Identical original double division.
                if(w==0.0) continue; // Spectrum is finite nonnegative for valid PCM.
                assert(count<kSparseCapacity);
                sparse_weight[count]=w;sparse_index[count]=static_cast<uint16_t>(k);++count;
            }
        }
        offsets[kMelBins]=static_cast<uint16_t>(count);
    }
    double weight(int i, int j) const {
        int left = edges[i], center = edges[i + 1], right = edges[i + 2];
        if (center <= left) center = left + 1;
        if (right <= center) right = center + 1;
        if (j >= left && j < center) return static_cast<double>(j - left) / std::max(1, center - left);
        if (j >= center && j < right) return static_cast<double>(right - j) / std::max(1, right - center);
        return 0.0;
    }
};

/* Iterative radix-2 DIT FFT (size 512 = 2^9). Forward sign exp(-2*pi*i*k*n/N),
 * matching numpy/scipy FFT. Self-contained: same code on host and on device. */
struct Fft512 {
    int rev[kNfft];
    double cos_t[kNfft / 2];
    double sin_t[kNfft / 2];
    Fft512() {
        const int logn = 9;
        for (int i = 0; i < kNfft; ++i) {
            int r = 0, x = i;
            for (int b = 0; b < logn; ++b) {
                r = (r << 1) | (x & 1);
                x >>= 1;
            }
            rev[i] = r;
        }
        for (int k = 0; k < kNfft / 2; ++k) {
            double ang = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(kNfft);
            cos_t[k] = std::cos(ang);
            sin_t[k] = std::sin(ang);
        }
    }
    void forward(double *re, double *im) const {
        for (int i = 0; i < kNfft; ++i) {
            int j = rev[i];
            if (j > i) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        for (int len = 2; len <= kNfft; len <<= 1) {
            int half = len >> 1;
            int step = kNfft / len;
            for (int i = 0; i < kNfft; i += len) {
                for (int k = 0; k < half; ++k) {
                    double wr = cos_t[k * step];
                    double wi = sin_t[k * step];
                    double xr = re[i + k + half];
                    double xi = im[i + k + half];
                    double tr,ti;
                    if(k==0) word23_f2_detail::twiddle_zero(xr,xi,tr,ti);
                    else {
                        tr = wr * xr - wi * xi;
                        ti = wr * xi + wi * xr;
                    }
                    re[i + k + half] = re[i + k] - tr;
                    im[i + k + half] = im[i + k] - ti;
                    re[i + k] += tr;
                    im[i + k] += ti;
                }
            }
        }
    }
};

const MelFilterbank &filterbank() { static const MelFilterbank fb; return fb; }

int compute_log_mel_frames(const void *pcm, bool is_i16, int frames,
                           int first_frame, int frame_stride, double *log_mel) {
    static const Window window;
    const MelFilterbank &fb=filterbank();
    static const Fft512 fft;
    const double scale2 = window.scale * window.scale;
    double re[kNfft];
    double im[kNfft];
    for (int t = first_frame; t < frames; t += frame_stride) {
        const size_t start = static_cast<size_t>(t) * kFrameShift;
        for (int n = 0; n < kNfft; ++n) { re[n] = 0.0; im[n] = 0.0; }
        for (int n = 0; n < kFrameLength; ++n) {
            const double sample = is_i16
                ? static_cast<const short *>(pcm)[start + n] / 32768.0
                : static_cast<const float *>(pcm)[start + n];
            re[n] = sample * window.w[n];
        }
        fft.forward(re, im);
        for (int k = 0; k < kSpectrumBins; ++k)
            re[k] = (re[k] * re[k] + im[k] * im[k]) * scale2;
        for (int m = 0; m < kMelBins; ++m) {
            double acc = 0.0;
            for(size_t j=fb.offsets[m];j<fb.offsets[m+1];++j)
                acc += fb.sparse_weight[j] * re[fb.sparse_index[j]];
            log_mel[static_cast<size_t>(m) * frames + t] = std::log(acc < kEps ? kEps : acc);
        }
    }
    return 0;
}


}  // namespace

static int log_mel_feature_impl(const void *pcm, bool is_i16, size_t sample_count,
    float *feature_out, void *workspace=nullptr) {
    if (sample_count < static_cast<size_t>(kFrameLength)) {
        return 1;
    }
    const int frames = static_cast<int>((sample_count - kFrameLength) / kFrameShift) + 1;

    /* log_mel[m * frames + t] */
    const size_t log_count=static_cast<size_t>(kMelBins)*frames;
    std::vector<double> log_mel_storage;
    double *log_mel;
    if(workspace) {
        log_mel=static_cast<double *>(workspace);
        // Begin scalar object lifetimes in the caller's raw arena. No value
        // initialization or allocation occurs; all cells are written below.
        for(size_t i=0;i<log_count;++i) ::new (log_mel+i) double;
    } else {
        log_mel_storage.resize(log_count);log_mel=log_mel_storage.data();
    }
    const int frame_rc = compute_log_mel_frames(pcm, is_i16, frames, 0, 1, log_mel);
    if (frame_rc) return frame_rc;

    /* Per-bin time statistics (population std, ddof=0, matching numpy default). */
    double mean_t[kMelBins];
    double std_t[kMelBins];
    double min_t[kMelBins];
    double max_t[kMelBins];
    for (int m = 0; m < kMelBins; ++m) {
        const double *row = &log_mel[static_cast<size_t>(m) * frames];
        double sum = 0.0;
        double mn = row[0];
        double mx = row[0];
        for (int t = 0; t < frames; ++t) {
            sum += row[t];
            mn = std::min(mn, row[t]);
            mx = std::max(mx, row[t]);
        }
        double mean = sum / frames;
        double var = 0.0;
        for (int t = 0; t < frames; ++t) {
            double d = row[t] - mean;
            var += d * d;
        }
        var /= frames;
        mean_t[m] = mean;
        std_t[m] = std::sqrt(var);
        min_t[m] = mn;
        max_t[m] = mx;
    }

    /* Per-bin time normalization + linear interpolation to 64 frames (np.interp,
     * source/target = linspace(0,1,.), endpoint-clamped). feature[m*64 + j]. */
    for (int m = 0; m < kMelBins; ++m) {
        const double *row = &log_mel[static_cast<size_t>(m) * frames];
        const double denom = std_t[m] + kNormEps;
        const double mean = mean_t[m];
        for (int j = 0; j < kFeatureFrames; ++j) {
            double x = (kFeatureFrames == 1) ? 0.0
                                             : static_cast<double>(j) / static_cast<double>(kFeatureFrames - 1);
            double pos = x * static_cast<double>(frames - 1);
            int i0 = static_cast<int>(std::floor(pos));
            double val;
            if (i0 >= frames - 1) {
                val = (row[frames - 1] - mean) / denom;
            } else if (i0 < 0) {
                val = (row[0] - mean) / denom;
            } else {
                double frac = pos - static_cast<double>(i0);
                double a = (row[i0] - mean) / denom;
                double b = (row[i0 + 1] - mean) / denom;
                val = a + frac * (b - a);
            }
            feature_out[m * kFeatureFrames + j] = static_cast<float>(val);
        }
    }

    /* Stats block (2560..2719): mean, std, min, max per bin (raw log_mel). */
    const int base = kMelBins * kFeatureFrames; /* 2560 */
    for (int m = 0; m < kMelBins; ++m) {
        feature_out[base + m] = static_cast<float>(mean_t[m]);
        feature_out[base + kMelBins + m] = static_cast<float>(std_t[m]);
        feature_out[base + 2 * kMelBins + m] = static_cast<float>(min_t[m]);
        feature_out[base + 3 * kMelBins + m] = static_cast<float>(max_t[m]);
    }
    return 0;
}

extern "C" int sts_f2_log_mel_feature(const float *pcm, size_t sample_count, float *feature_out) {
    return log_mel_feature_impl(pcm, false, sample_count, feature_out);
}

extern "C" int sts_f2_log_mel_feature_i16(const short *pcm, size_t sample_count, float *feature_out) {
    return log_mel_feature_impl(pcm, true, sample_count, feature_out);
}

extern "C" void sts_f2_normalize_feature(const float *feature, const float *mean, const float *std, float *out) {
    for (int i = 0; i < STS_FEAT_DIM; ++i) {
        out[i] = (feature[i] - mean[i]) / std[i];
    }
}

extern "C" size_t sts_f2_mel_cache_bytes() { return sizeof(MelFilterbank); }
extern "C" size_t sts_f2_mel_nonzero_weights() { return filterbank().offsets[kMelBins]; }
extern "C" size_t sts_f2_feature_static_bytes() { return sizeof(Window)+sizeof(MelFilterbank)+sizeof(Fft512); }

extern "C" size_t sts_f2_workspace_bytes(size_t sample_count) {
    if(sample_count<static_cast<size_t>(kFrameLength)) return 0;
    const size_t frames=(sample_count-kFrameLength)/kFrameShift+1;
    if(frames>static_cast<size_t>(std::numeric_limits<int>::max()) ||
        frames>std::numeric_limits<size_t>::max()/(kMelBins*sizeof(double))) return 0;
    return frames*kMelBins*sizeof(double);
}
static int feature_with_workspace(const void *pcm,bool is_i16,size_t sample_count,
    float *feature_out,void *workspace,size_t workspace_bytes) {
    if(sample_count<static_cast<size_t>(kFrameLength)) return 1;
    const size_t required=sts_f2_workspace_bytes(sample_count);
    if(!pcm || !feature_out || !workspace || required==0 || workspace_bytes<required ||
        reinterpret_cast<uintptr_t>(workspace)%16!=0) return 5;
    return log_mel_feature_impl(pcm,is_i16,sample_count,feature_out,workspace);
}
extern "C" int sts_f2_log_mel_feature_i16_with_workspace(const short *pcm,size_t count,
    float *out,void *workspace,size_t bytes) { return feature_with_workspace(pcm,true,count,out,workspace,bytes); }
extern "C" int sts_f2_log_mel_feature_with_workspace(const float *pcm,size_t count,
    float *out,void *workspace,size_t bytes) { return feature_with_workspace(pcm,false,count,out,workspace,bytes); }
