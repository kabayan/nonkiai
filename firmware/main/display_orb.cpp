#include "display_orb.h"
#include "orb_render.h"
#include <M5Unified.hpp>
#include <cmath>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {
M5Canvas canvas(&M5.Display);
bool attempted = false, ready = false, failed = false;
uint16_t *colors = nullptr;
float level = 0, animation_s = 0, breath_phase = 0, ripple_phase = 0;
float sleep_mix = 0, sleep_s = 0;
uint32_t last_ms = 0;
nk::OrbPalette palette = nk::orb_target(nk::OrbState::Idle, 0, 0);
uint16_t mix(float a, int r, int g, int b) {
    return canvas.color565(8 + static_cast<int>((r-8)*a),
                           11 + static_cast<int>((g-11)*a),
                           28 + static_cast<int>((b-28)*a));
}
}

void nk_display_orb_reset_failure() {
    failed = false;
}

bool nk_display_orb_draw(uint32_t now_ms, bool asleep, bool happy, bool mouth_open,
                         nk::OrbState state, int posture_state, int angle_cdeg) {
    if (failed) return false;
    if (!attempted) {
        attempted = true;
        canvas.setPsram(true);
        canvas.setColorDepth(16);
        void *buffer = canvas.createSprite(128, 128);
        // LovyanGFX may fall back to internal RAM: never keep that allocation.
        if (buffer && esp_ptr_external_ram(buffer))
            colors = static_cast<uint16_t *>(heap_caps_malloc(2048*sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        ready = colors != nullptr;
        if (!ready) {
            canvas.deleteSprite();
            attempted = false;
            failed = true;
            ESP_LOGW("nk-orb", "[ORB] PSRAM canvas allocation failed; using face");
        }
        last_ms = now_ms;
    }
    if (!ready) return false;
    const int64_t start = esp_timer_get_time();
    // Integrate speed to keep the animation continuous on sleep/wake transitions.
    // DLS-178: 位相を保ち、眠りの見た目だけ滑らかに切り替える。
    const float real_dt = (now_ms-last_ms)*0.001f;
    sleep_mix = nk::orb_sleep_follow(sleep_mix, asleep, real_dt);
    sleep_s = std::fmod(sleep_s+real_dt, 3.0f);
    const float dt = real_dt*(1.0f-0.5f*sleep_mix);
    animation_s += dt;
    last_ms = now_ms;
    // Integrate per-state speeds so listening/speaking transitions keep the phase continuous.
    const bool active = state != nk::OrbState::Idle;
    breath_phase = std::fmod(breath_phase+dt*(active ? 2.6f : 1.4f), 6.2831853f);
    ripple_phase = std::fmod(ripple_phase+dt/(active ? 1.2f : 2.5f), 1.0f);
    palette = nk::orb_follow(palette, nk::orb_target(state, posture_state, angle_cdeg));
    const auto light = nk::orb_sleep_palette(palette, sleep_mix);
    const auto center = light.center;
    const auto inner = light.inner;
    const auto outer = light.outer;
    const auto ripple = light.ripple;
    const float awake = 1.0f-sleep_mix;
    const float scale = 1.0f-0.4f*sleep_mix;
    const float t = animation_s;
    const float target = mouth_open || happy ? 1.0f : 0.0f;
    level += (target-level)*(target > level ? 0.35f : 0.08f);
    const float breath = std::sin(breath_phase);
    const float pulse = level*awake*(9.0f+3.0f*std::sin(t*9.0f));
    const float r = (26.0f+breath*6.0f+pulse)*scale;
    const float cx = 64+(7*std::sin(t*0.57f)+3*std::sin(t*1.13f))*(1.0f-0.8f*sleep_mix);
    const float cy = 64+(7*std::cos(t*0.43f)+3*std::sin(t*0.91f))*(1.0f-0.8f*sleep_mix);
    const float glow_r = r+12*scale;
    const uint16_t bg = canvas.color565(8,11,28);
    canvas.fillScreen(bg);
    // Whiten only the core; the rings and halo retain the state/tilt palette.
    const nk::OrbRgb white{255,255,255};
    const auto core = nk::orb_mix(center, white, (0.45f+level*awake*0.4f)*awake);
    nk::orb_glow_table(colors, core);
    nk::orb_write_glow(static_cast<uint16_t *>(canvas.getBuffer()), colors, cx, cy, glow_r);
    const int x = static_cast<int>(std::lround(cx)), y = static_cast<int>(std::lround(cy));
    const int inner_r = static_cast<int>(std::lround(r+(3-breath*1.5f)*scale));
    const int outer_r = static_cast<int>(std::lround(r+(10-breath*2.0f)*scale));
    for (int width = 0; width < 3; ++width) {
        canvas.drawCircle(x,y,inner_r-width,canvas.color565(inner.r,inner.g,inner.b));
        canvas.drawCircle(x,y,outer_r-width,canvas.color565(outer.r,outer.g,outer.b));
    }
    for (int ring = 0; sleep_mix < 0.99f && ring < 3; ++ring) {
        const float phase = std::fmod(ripple_phase+ring/3.0f,1.0f);
        const int radius = static_cast<int>(std::lround(4+phase*104));
        // 灯に重なる波紋で暗い線を作らない。
        if (sleep_mix > 0 && radius <= glow_r+2) continue;
        const uint16_t color = mix((1-phase)*0.85f*awake,ripple.r,ripple.g,ripple.b);
        for (int width = 0; width < 3; ++width)
            canvas.drawCircle(x,y,radius-width,color);
    }
    for (int particle = 0; sleep_mix < 0.99f && particle < 7; ++particle) {
        const float angle = t*0.85f+particle*(6.2831853f/7);
        const float orbit = r+15+3*std::sin(t*1.7f+particle);
        const float twinkle = 0.55f+0.45f*std::sin(t*3.7f+particle*2.3f);
        const auto tint = nk::orb_mix(particle%2 ? inner : ripple, white, 0.35f);
        canvas.fillCircle(static_cast<int>(std::lround(cx+std::cos(angle)*orbit)),
                          static_cast<int>(std::lround(cy+std::sin(angle)*orbit)),
                          twinkle > 0.65f ? 2 : 1, mix(twinkle*awake,tint.r,tint.g,tint.b));
    }
    // DLS-184: 外向きを上にした z を12方向へ浮かべる。
    if (sleep_mix >= 0.5f) {
        for (int i = 0; i < nk::kOrbSleepZCount; ++i) {
            const auto z = nk::orb_sleep_z(sleep_s, i, cx, cy, glow_r);
            const uint16_t color = mix(z.alpha*(2*sleep_mix-1),110,140,220);
            const auto offset = nk::orb_sleep_rotate({0, 1}, i);
            for (int line = 0; line < 3; ++line) {
                const auto from = z.points[line];
                const auto to = z.points[line+1];
                for (int width = 0; width < 2; ++width)
                    canvas.drawLine(static_cast<int>(std::lround(from.x+width*offset.x)),
                                    static_cast<int>(std::lround(from.y+width*offset.y)),
                                    static_cast<int>(std::lround(to.x+width*offset.x)),
                                    static_cast<int>(std::lround(to.y+width*offset.y)), color);
            }
        }
    }
    canvas.pushSprite(0,0);
    const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time()-start);
    static uint32_t frames = 0, max_us = 0;
    static uint64_t total_us = 0;
    static int64_t window_us = start;
    ++frames; total_us += elapsed;
    if (elapsed > max_us) max_us = elapsed;
    if (start-window_us >= 10000000) {
        ESP_LOGI("nk-orb", "[ORB] frames=%lu avg=%.1fus max=%luus", (unsigned long)frames,
                 static_cast<double>(total_us)/frames, (unsigned long)max_us);
        frames = max_us = 0; total_us = 0; window_us = start;
    }
    return true;
}
