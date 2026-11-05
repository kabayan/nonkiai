#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t code;
} nk_weather_t;

typedef struct {
    nk_weather_t today;
    nk_weather_t tomorrow;
} nk_weather_forecast_t;

// Start each boot without a forecast. No NVS writes are made.
void nk_weather_init(void);

// Both supported WMO codes and consecutive Tokyo dates must be valid.
// Either both days replace the previous forecast or neither does.
bool nk_weather_set_forecast(const nk_weather_forecast_t *forecast);

// Returns false when nothing was received, the clock is unsynced, or the
// stored date has expired. The caller's output is untouched on false.
bool nk_weather_get_today(nk_weather_t *out);
bool nk_weather_get_tomorrow(nk_weather_t *out);

// Read both slots against one clock snapshot, for a coherent BLE state reply.
// Unknown slots in out are zeroed. All three pointers must be non-null.
void nk_weather_get_current(nk_weather_forecast_t *out, bool *today_known, bool *tomorrow_known);

// A ready-to-say description of today's forecast, or exactly "わからないの".
const char *nk_weather_today_text(void);
const char *nk_weather_tomorrow_text(void);

#ifdef __cplusplus
}
#endif
