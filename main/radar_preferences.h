#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { RADAR_POLL_MIN_SECONDS = 5, RADAR_POLL_MAX_SECONDS = 600 };
#define RADAR_STALE_CUTOFF_MS INT64_C(60000)

static inline int radar_clamp_poll_seconds(int seconds)
{
    if (seconds < RADAR_POLL_MIN_SECONDS) return RADAR_POLL_MIN_SECONDS;
    if (seconds > RADAR_POLL_MAX_SECONDS) return RADAR_POLL_MAX_SECONDS;
    return seconds;
}

static inline bool radar_stale_expired(int64_t first_failure_ms, int64_t now_ms)
{
    return first_failure_ms >= 0 && now_ms >= first_failure_ms &&
           now_ms - first_failure_ms >= RADAR_STALE_CUTOFF_MS;
}
