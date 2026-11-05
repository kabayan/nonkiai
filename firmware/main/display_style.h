#pragma once
#include <stdint.h>
enum class DisplayStyle : uint8_t { Face = 0, Orb = 1, Face90 = 2, Face180 = 3, Face270 = 4, Count };
inline uint8_t nk_display_style_valid(uint8_t style) {
    return style < static_cast<uint8_t>(DisplayStyle::Count) ? style : 0;
}
inline uint8_t nk_display_style_next(uint8_t style) {
    switch (static_cast<DisplayStyle>(nk_display_style_valid(style))) {
    case DisplayStyle::Face: return static_cast<uint8_t>(DisplayStyle::Face90);
    case DisplayStyle::Face90: return static_cast<uint8_t>(DisplayStyle::Face180);
    case DisplayStyle::Face180: return static_cast<uint8_t>(DisplayStyle::Face270);
    case DisplayStyle::Face270: return static_cast<uint8_t>(DisplayStyle::Orb);
    default: return static_cast<uint8_t>(DisplayStyle::Face);
    }
}
inline bool nk_display_style_is_face(uint8_t style) {
    return nk_display_style_valid(style) != static_cast<uint8_t>(DisplayStyle::Orb);
}
inline uint8_t nk_display_style_face_quarter(uint8_t style) {
    style = nk_display_style_valid(style);
    return style >= static_cast<uint8_t>(DisplayStyle::Face90) ? style - 1 : 0;
}
