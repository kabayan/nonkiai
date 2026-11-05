#pragma once
#include <algorithm>
#include <cmath>

namespace nk {
struct OrbRgb { float r, g, b; };
struct OrbPalette { OrbRgb center, inner, outer, ripple; };
enum class OrbState { Idle, Listening, Speaking };
inline OrbState orb_state(bool playing, bool sampling, bool capturing) {
    // Recording/inference is listening; Reply/Failed waiting is excluded.
    // Playback, including start/end cues, takes priority over capture.
    return playing ? OrbState::Speaking : (sampling || capturing ? OrbState::Listening : OrbState::Idle);
}
inline OrbRgb orb_mix(OrbRgb from, OrbRgb to, float amount) {
    return {from.r+(to.r-from.r)*amount, from.g+(to.g-from.g)*amount, from.b+(to.b-from.b)*amount};
}
inline OrbPalette orb_mix(OrbPalette from, OrbPalette to, float amount) {
    return {orb_mix(from.center,to.center,amount), orb_mix(from.inner,to.inner,amount),
            orb_mix(from.outer,to.outer,amount), orb_mix(from.ripple,to.ripple,amount)};
}
inline OrbPalette orb_target(OrbState state, int posture_state, int angle_cdeg) {
    if (state == OrbState::Speaking) return {{255,205,150},{230,160,110},{130,80,55},{200,130,90}};
    if (state == OrbState::Listening) return {{170,255,200},{110,210,160},{50,110,80},{90,190,140}};
    const OrbPalette lavender{{215,221,255},{135,180,225},{55,80,125},{93,136,178}};
    const OrbPalette blue{{170,235,255},{100,195,225},{45,90,125},{80,155,190}};
    const OrbPalette pink{{255,190,230},{220,135,195},{120,55,100},{185,100,155}};
    const int angle = posture_state == 0 || angle_cdeg < 0 ? 0 : (angle_cdeg > 9000 ? 9000 : angle_cdeg);
    return angle <= 4500 ? orb_mix(lavender,blue,angle/4500.0f)
                         : orb_mix(blue,pink,(angle-4500)/4500.0f);
}
inline OrbPalette orb_follow(OrbPalette current, OrbPalette target) {
    return orb_mix(current,target,0.15f);
}
// DLS-178: 実時間で約1秒かけて眠りへ寄せる。
inline float orb_sleep_follow(float current, bool asleep, float dt) {
    const float step = std::max(0.0f, dt);
    return asleep ? std::min(1.0f, current+step) : std::max(0.0f, current-step);
}
inline OrbPalette orb_sleep_palette(OrbPalette awake, float sleep_mix) {
    return orb_mix(awake, {{40,60,130},{32,48,108},{24,36,82},{32,48,108}},
                   std::clamp(sleep_mix, 0.0f, 1.0f));
}
struct OrbSleepPoint { float x, y; };
struct OrbSleepZ {
    float x, y, alpha, size;
    OrbSleepPoint points[4];
};
inline constexpr int kOrbSleepZCount = 12;
// DLS-184: 画面座標で上から時計回りに30度ずつ回す。
inline OrbSleepPoint orb_sleep_rotate(OrbSleepPoint p, int direction) {
    const float angle = direction*(3.14159265358979323846f/6.0f);
    const float c = std::cos(angle), s = std::sin(angle);
    return {p.x*c-p.y*s, p.x*s+p.y*c};
}
// DLS-184: 最終サイズと太さの外接範囲から、余白1pxを残す終点を求める。
inline float orb_sleep_finish(int direction, float cx, float cy) {
    const auto outward = orb_sleep_rotate({0, -1}, direction);
    const auto down = orb_sleep_rotate({0, 1}, direction);
    const auto corner = orb_sleep_rotate({9, 9}, direction);
    const auto other = orb_sleep_rotate({9, -9}, direction);
    const float ex = std::max(std::abs(corner.x), std::abs(other.x));
    const float ey = std::max(std::abs(corner.y), std::abs(other.y));
    float finish = 256.0f;
    const auto limit = [&finish](float center, float axis, float extent, float offset) {
        if (axis > 0.000001f)
            finish = std::min(finish, (126-center-extent-std::max(0.0f, offset))/axis);
        else if (axis < -0.000001f)
            finish = std::min(finish, (1-center+extent-std::min(0.0f, offset))/axis);
    };
    limit(cx, outward.x, ex, down.x);
    limit(cy, outward.y, ey, down.y);
    return finish;
}
// DLS-184: 光の外から12方向へ、外に行くほど大きな字を流す。
inline OrbSleepZ orb_sleep_z(float seconds, int direction, float cx, float cy,
                            float glow_radius) {
    // DLS-184: 先に開始時刻を引き、周期境界の丸めによる明滅を避ける。
    const float cycle = (seconds-direction*(3.0f/kOrbSleepZCount))/3.0f;
    const float phase = cycle-std::floor(cycle);
    const float size = 4.0f+14.0f*phase;
    const float half = size/2.0f;
    const auto outward = orb_sleep_rotate({0, -1}, direction);
    const float finish = orb_sleep_finish(direction, cx, cy);
    const float start = std::min(glow_radius+5.0f, finish);
    const float distance = start+(finish-start)*phase;
    OrbSleepZ z{cx+outward.x*distance, cy+outward.y*distance,
                4.0f*phase*(1.0f-phase), size, {}};
    const OrbSleepPoint local[4] = {{-half,-half}, {half,-half},
                                   {-half,half}, {half,half}};
    for (int i = 0; i < 4; ++i) {
        const auto p = orb_sleep_rotate(local[i], direction);
        z.points[i] = {z.x+p.x, z.y+p.y};
    }
    return z;
}
inline OrbRgb orb_dim(OrbRgb color, bool asleep) {
    const float scale = asleep ? 0.5f : 1.0f;
    return {color.r*scale,color.g*scale,color.b*scale};
}
}
