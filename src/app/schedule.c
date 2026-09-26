/**
 * @file schedule.c
 * @brief Weekly program in the week_program_2 layout.
 */

#include "schedule.h"

#define TEMP_MIN_X10 50
#define TEMP_MAX_X10 350

static const uint8_t *point(const uint8_t *s, uint8_t day, uint8_t idx)
{
    return s + (day * SCHED_POINTS + idx) * 4;
}

void schedule_get_point(const uint8_t *s, uint8_t day, uint8_t idx, uint8_t *h, uint8_t *m, int16_t *x10)
{
    const uint8_t *p = point(s, day, idx);
    *h               = p[0];
    *m               = p[1];
    *x10             = (int16_t)((p[2] << 8) | p[3]);
}

void schedule_set_point(uint8_t *s, uint8_t day, uint8_t idx, uint8_t h, uint8_t m, int16_t x10)
{
    uint8_t *p = (uint8_t *)point(s, day, idx);
    p[0]       = h;
    p[1]       = m;
    p[2]       = (uint8_t)((uint16_t)x10 >> 8);
    p[3]       = (uint8_t)x10;
}

void schedule_default(uint8_t *s)
{
    /* wake / leave / return / sleep, same every day */
    static const struct {
        uint8_t h, m;
        int16_t t;
    } def[SCHED_POINTS] = {{6, 0, 210}, {8, 30, 180}, {17, 0, 210}, {22, 30, 180}};

    for (uint8_t d = 0; d < SCHED_DAYS; d++) {
        for (uint8_t i = 0; i < SCHED_POINTS; i++) {
            schedule_set_point(s, d, i, def[i].h, def[i].m, def[i].t);
        }
    }
}

bool schedule_validate(const uint8_t *s, size_t len)
{
    if (len != SCHED_BYTES) {
        return false;
    }
    for (uint8_t d = 0; d < SCHED_DAYS; d++) {
        for (uint8_t i = 0; i < SCHED_POINTS; i++) {
            uint8_t h, m;
            int16_t t;
            schedule_get_point(s, d, i, &h, &m, &t);
            if (h > 23 || m > 59 || t < TEMP_MIN_X10 || t > TEMP_MAX_X10) {
                return false;
            }
        }
    }
    return true;
}

/* Latest point of `day` at or before `minute` (points need not be sorted). */
static bool latest_before(const uint8_t *s, uint8_t day, uint16_t minute, int16_t *x10)
{
    int best = -1;
    for (uint8_t i = 0; i < SCHED_POINTS; i++) {
        uint8_t h, m;
        int16_t t;
        schedule_get_point(s, day, i, &h, &m, &t);
        int at = h * 60 + m;
        if (at <= minute && at > best) {
            best = at;
            *x10 = t;
        }
    }
    return best >= 0;
}

bool schedule_target(const uint8_t *s, uint8_t weekday, uint16_t minute, int16_t *x10)
{
    if (weekday >= SCHED_DAYS) {
        return false;
    }
    if (latest_before(s, weekday, minute, x10)) {
        return true;
    }
    /* before today's first point: the last point of an earlier day holds */
    for (uint8_t back = 1; back <= SCHED_DAYS; back++) {
        uint8_t day = (uint8_t)((weekday + SCHED_DAYS - back) % SCHED_DAYS);
        if (latest_before(s, day, 24 * 60 - 1, x10)) {
            return true;
        }
    }
    return false;
}
