#include "device_weather.h"

#include <time.h>

#include "device_clock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

namespace {
portMUX_TYPE s_weather_lock = portMUX_INITIALIZER_UNLOCKED;
nk_weather_forecast_t s_forecast = {};
bool s_known = false;

const char *description(uint8_t code)
{
    switch (code) {
        case 0: return "晴れなの";
        case 1: return "おおむね晴れなの";
        case 2: return "晴れとくもりなの";
        case 3: return "くもりなの";
        case 45: case 48: return "霧なの";
        case 51: case 53: case 55: return "霧雨なの";
        case 56: case 57: return "凍る霧雨なの";
        case 61: return "小雨なの";
        case 63: return "雨なの";
        case 65: return "強い雨なの";
        case 66: case 67: return "凍る雨なの";
        case 71: return "小雪なの";
        case 73: return "雪なの";
        case 75: return "強い雪なの";
        case 77: return "雪粒なの";
        case 80: case 81: case 82: return "にわか雨なの";
        case 85: case 86: return "にわか雪なの";
        case 95: return "雷雨なの";
        case 96: case 99: return "雷雨とひょうなの";
        default: return nullptr;
    }
}

bool valid_date(uint16_t year, uint8_t month, uint8_t day)
{
    if (year < 2000 || year > 2099 || month < 1 || month > 12) return false;
    static const uint8_t days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const uint8_t last_day = days[month] + (month == 2 && leap ? 1 : 0);
    return day >= 1 && day <= last_day;
}

bool tokyo_date(int64_t utc_ms, uint8_t days_ahead, uint16_t *year, uint8_t *month, uint8_t *day)
{
    if (utc_ms < 0) return false;
    const time_t tokyo_seconds = static_cast<time_t>(utc_ms / 1000 + 9 * 60 * 60 +
                                                       static_cast<int64_t>(days_ahead) * 24 * 60 * 60);
    struct tm date;
    if (gmtime_r(&tokyo_seconds, &date) == nullptr) return false;
    *year = static_cast<uint16_t>(date.tm_year + 1900);
    *month = static_cast<uint8_t>(date.tm_mon + 1);
    *day = static_cast<uint8_t>(date.tm_mday);
    return true;
}

bool matches_date(const nk_weather_t &weather, int64_t utc_ms, uint8_t days_ahead)
{
    uint16_t year;
    uint8_t month, day;
    return tokyo_date(utc_ms, days_ahead, &year, &month, &day) &&
           weather.year == year && weather.month == month && weather.day == day;
}

bool get_day(uint8_t days_ahead, nk_weather_t *out)
{
    if (out == nullptr) return false;
    nk_weather_forecast_t current;
    bool today_known, tomorrow_known;
    nk_weather_get_current(&current, &today_known, &tomorrow_known);
    if (days_ahead == 0 && !today_known) return false;
    if (days_ahead == 1 && !tomorrow_known) return false;
    *out = days_ahead == 0 ? current.today : current.tomorrow;
    return true;
}
}  // namespace

void nk_weather_init(void)
{
    portENTER_CRITICAL(&s_weather_lock);
    s_forecast = {};
    s_known = false;
    portEXIT_CRITICAL(&s_weather_lock);
}

bool nk_weather_set_forecast(const nk_weather_forecast_t *forecast)
{
    const int64_t utc_ms = nk_clock_now_utc_ms();
    if (forecast == nullptr ||
        !valid_date(forecast->today.year, forecast->today.month, forecast->today.day) ||
        !valid_date(forecast->tomorrow.year, forecast->tomorrow.month, forecast->tomorrow.day) ||
        description(forecast->today.code) == nullptr || description(forecast->tomorrow.code) == nullptr ||
        !matches_date(forecast->today, utc_ms, 0) ||
        !matches_date(forecast->tomorrow, utc_ms, 1)) return false;

    portENTER_CRITICAL(&s_weather_lock);
    s_forecast = *forecast;
    s_known = true;
    portEXIT_CRITICAL(&s_weather_lock);
    return true;
}

bool nk_weather_get_today(nk_weather_t *out)
{
    return get_day(0, out);
}

bool nk_weather_get_tomorrow(nk_weather_t *out) { return get_day(1, out); }

void nk_weather_get_current(nk_weather_forecast_t *out, bool *today_known, bool *tomorrow_known)
{
    *out = {};
    *today_known = false;
    *tomorrow_known = false;
    const int64_t utc_ms = nk_clock_now_utc_ms();
    if (utc_ms < 0) return;

    nk_weather_forecast_t saved;
    bool known;
    portENTER_CRITICAL(&s_weather_lock);
    saved = s_forecast;
    known = s_known;
    portEXIT_CRITICAL(&s_weather_lock);
    if (!known) return;

    for (uint8_t offset = 0; offset <= 1; ++offset) {
        nk_weather_t *slot = offset == 0 ? &out->today : &out->tomorrow;
        bool *slot_known = offset == 0 ? today_known : tomorrow_known;
        if (matches_date(saved.today, utc_ms, offset)) {
            *slot = saved.today;
            *slot_known = true;
        } else if (matches_date(saved.tomorrow, utc_ms, offset)) {
            *slot = saved.tomorrow;
            *slot_known = true;
        }
    }
}

const char *nk_weather_today_text(void)
{
    nk_weather_t weather;
    if (!nk_weather_get_today(&weather)) return "わからないの";
    const char *text = description(weather.code);
    return text != nullptr ? text : "わからないの";
}

const char *nk_weather_tomorrow_text(void)
{
    nk_weather_t weather;
    if (!nk_weather_get_tomorrow(&weather)) return "わからないの";
    const char *text = description(weather.code);
    return text != nullptr ? text : "わからないの";
}
