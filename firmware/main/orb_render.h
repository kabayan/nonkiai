#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "orb_color.h"

namespace nk {
constexpr int orb_color_count = 2048;
// LovyanGFX's 16-bit sprite stores swap565, not native RGB565.
constexpr uint16_t orb_swap565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xf8) | (g >> 5)) |
                                 (((g << 3) & 0xe0) | (b >> 3)) << 8);
}
struct OrbRect { int left, top, right, bottom; }; // Half-open bounds.
inline OrbRect orb_clip_rect(int left, int top, int right, int bottom) {
    return {std::clamp(left, 0, 128), std::clamp(top, 0, 128),
            std::clamp(right, 0, 128), std::clamp(bottom, 0, 128)};
}
inline void orb_glow_table(uint16_t *table, OrbRgb center) {
    for (int i = 0; i < orb_color_count; ++i) {
        const float distance2 = i / static_cast<float>(orb_color_count - 1);
        // A broad bright core with a smooth, zero-slope falloff into the halo.
        const float strength = (1-distance2)*(1-distance2)*(1-distance2);
        table[i] = orb_swap565(static_cast<uint8_t>(8+(center.r-8)*strength),
                               static_cast<uint8_t>(11+(center.g-11)*strength),
                               static_cast<uint8_t>(28+(center.b-28)*strength));
    }
}
inline void orb_write_glow(uint16_t *buffer, const uint16_t *table,
                           float cx, float cy, float radius) {
    const auto bounds = orb_clip_rect(static_cast<int>(std::floor(cx-radius)),
        static_cast<int>(std::floor(cy-radius)),
        static_cast<int>(std::ceil(cx+radius))+1,
        static_cast<int>(std::ceil(cy+radius))+1);
    const float radius2 = radius*radius;
    const float scale = (orb_color_count-1)/radius2;
    for (int y = bounds.top; y < bounds.bottom; ++y) {
        const float dy = y-cy;
        auto *row = buffer+y*128;
        for (int x = bounds.left; x < bounds.right; ++x) {
            const float dx = x-cx;
            const float d2 = dx*dx+dy*dy;
            if (d2 <= radius2) {
                const int index = std::min(orb_color_count-1, static_cast<int>(d2*scale));
                row[x] = table[index];
            }
        }
    }
}
}
