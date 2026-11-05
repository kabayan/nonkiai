#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns false for an implausible timestamp or UTC offset. No flash writes.
bool nk_clock_sync(int64_t unix_ms, int16_t utc_offset_minutes);

// -1 means this boot has not received a valid time sync.
int64_t nk_clock_now_utc_ms(void);
int64_t nk_clock_now_local_ms(void);

#ifdef __cplusplus
}
#endif
