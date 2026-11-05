#include "device_clock.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

namespace {
portMUX_TYPE s_clock_lock = portMUX_INITIALIZER_UNLOCKED;
int64_t s_unix_ms_at_sync = -1;
int64_t s_uptime_us_at_sync = 0;
int16_t s_utc_offset_minutes = 0;

int64_t now_ms(bool local)
{
    int64_t unix_ms;
    int64_t uptime_us;
    int16_t offset_minutes;
    portENTER_CRITICAL(&s_clock_lock);
    unix_ms = s_unix_ms_at_sync;
    uptime_us = s_uptime_us_at_sync;
    offset_minutes = s_utc_offset_minutes;
    portEXIT_CRITICAL(&s_clock_lock);
    if (unix_ms < 0) return -1;
    const int64_t elapsed_ms = (esp_timer_get_time() - uptime_us) / 1000;
    return unix_ms + elapsed_ms + (local ? static_cast<int64_t>(offset_minutes) * 60000 : 0);
}
}  // namespace

bool nk_clock_sync(int64_t unix_ms, int16_t utc_offset_minutes)
{
    // 2000-01-01 through 2100-01-01; reject a device clock that is clearly unset.
    if (unix_ms < 946684800000LL || unix_ms >= 4102444800000LL ||
        utc_offset_minutes < -840 || utc_offset_minutes > 840) return false;
    const int64_t uptime_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_clock_lock);
    s_unix_ms_at_sync = unix_ms;
    s_uptime_us_at_sync = uptime_us;
    s_utc_offset_minutes = utc_offset_minutes;
    portEXIT_CRITICAL(&s_clock_lock);
    return true;
}

int64_t nk_clock_now_utc_ms(void) { return now_ms(false); }
int64_t nk_clock_now_local_ms(void) { return now_ms(true); }
