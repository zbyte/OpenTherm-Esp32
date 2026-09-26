/**
 * @file clock.c
 * @brief Local wall clock for the schedule.
 */

#include "clock.h"

#include "tal_api.h"
#include "tuya_kconfig.h"

/* Anything before 2025-01-01 is the unset RTC counting from zero. */
#define MIN_VALID_EPOCH 1735689600u

static bool s_manual;

void clock_init(void)
{
    /* The cloud overwrites this with the user's zone on sync. */
    if (tal_time_check_time_zone_sync() != OPRT_OK) {
        tal_time_set_time_zone_seconds(HEAT_DEFAULT_TZ_MIN * 60);
    }
}

void clock_set_utc(uint32_t epoch)
{
    tal_time_set_posix((TIME_T)epoch, 1);
    s_manual = true;
}

bool clock_now(uint8_t *weekday, uint16_t *minute)
{
    if (!s_manual && tal_time_check_time_sync() != OPRT_OK) {
        return false;
    }
    TIME_T now = tal_time_get_posix();
    if (now < MIN_VALID_EPOCH) {
        return false;
    }

    POSIX_TM_S tm;
    if (tal_time_get_local_time_custom(now, &tm) != OPRT_OK) {
        return false;
    }
    *weekday = (uint8_t)((tm.tm_wday + 6) % 7); /* tm: 0 = Sunday */
    *minute  = (uint16_t)(tm.tm_hour * 60 + tm.tm_min);
    return true;
}
