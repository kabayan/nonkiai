#pragma once
#include <stdint.h>
#include "orb_color.h"
// False if PSRAM canvas could not be allocated; caller draws a face fallback.
bool nk_display_orb_draw(uint32_t now_ms, bool asleep, bool happy, bool mouth_open,
                         nk::OrbState state, int posture_state, int angle_cdeg);
// Permit another allocation attempt when the user selects Orb again.
void nk_display_orb_reset_failure();
