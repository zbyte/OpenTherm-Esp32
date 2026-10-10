/**
 * @file clock.c
 * @brief Local wall clock for the schedule.
 */

#include "clock.h"

#include <stdio.h>

#include "tal_api.h"

#include "settings.h"

/* Anything before 2025-01-01 is the unset RTC counting from zero. */
#define MIN_VALID_EPOCH 1735689600u

static bool     s_manual;
static bool     s_synced; /* SNTP answered at least once */
static uint32_t s_sync_ms;

void clock_init(void)
{
    settings_t c;
    settings_get(&c);
    clock_set_tz(c.tz_min);
}

void clock_set_tz(int16_t minutes)
{
    tal_time_set_time_zone_seconds(minutes * 60);
}

void clock_set_utc(uint32_t epoch)
{
    tal_time_set_posix((TIME_T)epoch, 1);
    s_manual = true;
}

void clock_set_utc_synced(uint32_t epoch)
{
    if (epoch >= MIN_VALID_EPOCH) {
        tal_time_set_posix((TIME_T)epoch, 1);
        s_sync_ms = tal_system_get_millisecond();
        s_synced  = true;
    }
}

bool clock_sync_age(uint32_t *seconds)
{
    if (!s_synced) {
        return false;
    }
    *seconds = (tal_system_get_millisecond() - s_sync_ms) / 1000;
    return true;
}

static bool local_tm(POSIX_TM_S *tm)
{
    if (!s_manual && tal_time_check_time_sync() != OPRT_OK) {
        return false;
    }
    TIME_T now = tal_time_get_posix();
    return now >= MIN_VALID_EPOCH && tal_time_get_local_time_custom(now, tm) == OPRT_OK;
}

bool clock_format(char *buf, size_t len)
{
    POSIX_TM_S tm;
    if (!local_tm(&tm)) {
        snprintf(buf, len, "unknown");
        return false;
    }
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec);
    return true;
}

bool clock_now(uint8_t *weekday, uint16_t *minute)
{
    POSIX_TM_S tm;
    if (!local_tm(&tm)) {
        return false;
    }
    *weekday = (uint8_t)((tm.tm_wday + 6) % 7); /* tm: 0 = Sunday */
    *minute  = (uint16_t)(tm.tm_hour * 60 + tm.tm_min);
    return true;
}
