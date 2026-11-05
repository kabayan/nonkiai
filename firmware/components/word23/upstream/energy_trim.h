#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Port of tools/voicebase_acoustic/human_trim.py POLICY, including linear p20.
// No activity retains the full input and is reported separately from inference.
struct TrimBounds {
    int start, end, active_frames;
    double noise_dbfs, threshold_dbfs, peak_dbfs;
};
inline TrimBounds energy_trim(const int16_t *pcm, int samples) {
    // Capacity also covers the historical <=50880-sample trim regression.
    // Live app supplies exactly 48000 samples.
    constexpr int frame = 320, max_frames = 160, padding = 1600;
    const int frames = std::min(samples / frame, max_frames);
    TrimBounds b{0, samples, 0, -240., -40., -240.};
    if (frames < 2) return b;
    double levels[max_frames], ordered[max_frames];
    bool sustained[max_frames]{};
    for (int f = 0; f < frames; ++f) {
        double sum = 0.;
        for (int i = 0; i < frame; ++i) {
            const double x = pcm[f * frame + i] / 32768.;
            sum += x * x;
        }
        levels[f] = 20. * std::log10(std::max(std::sqrt(sum / frame), 1e-12));
        ordered[f] = levels[f];
        b.peak_dbfs = std::max(b.peak_dbfs, levels[f]);
    }
    std::sort(ordered, ordered + frames);
    const double rank = (frames - 1) * .2;
    const int low = static_cast<int>(rank), high = std::min(low + 1, frames - 1);
    b.noise_dbfs = ordered[low] + (ordered[high] - ordered[low]) * (rank - low);
    b.threshold_dbfs = std::max({b.noise_dbfs + 10., b.peak_dbfs - 25., -40.});
    for (int f = 0; f + 1 < frames; ++f)
        if (levels[f] >= b.threshold_dbfs && levels[f + 1] >= b.threshold_dbfs)
            sustained[f] = sustained[f + 1] = true;
    int first = -1, last = -1;
    for (int f = 0; f < frames; ++f) if (sustained[f]) {
        if (first < 0) first = f;
        last = f;
        ++b.active_frames;
    }
    if (first >= 0) {
        b.start = std::max(0, first * frame - padding);
        b.end = std::min(samples, (last + 1) * frame + padding);
    }
    return b;
}
