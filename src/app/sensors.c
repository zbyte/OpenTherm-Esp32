/**
 * @file sensors.c
 * @brief Room temperature sensor slots and control-sensor selection.
 */

#include "sensors.h"

#include "tal_api.h"

#define STALE_AFTER_MS (5 * 60 * 1000)

typedef struct {
    bool     seen;
    int16_t  x10;
    uint32_t at_ms;
} slot_t;

static MUTEX_HANDLE s_lock;
static slot_t       s_slots[SENSOR_SLOTS];

void sensors_init(void)
{
    tal_mutex_create_init(&s_lock);
}

void sensors_set(uint8_t slot, int16_t x10)
{
    if (slot >= SENSOR_SLOTS) {
        return;
    }
    tal_mutex_lock(s_lock);
    s_slots[slot] = (slot_t){.seen = true, .x10 = x10, .at_ms = tal_system_get_millisecond()};
    tal_mutex_unlock(s_lock);
}

bool sensors_get(uint8_t slot, int16_t *x10)
{
    if (slot >= SENSOR_SLOTS) {
        return false;
    }
    tal_mutex_lock(s_lock);
    slot_t s = s_slots[slot];
    tal_mutex_unlock(s_lock);

    if (!s.seen || tal_system_get_millisecond() - s.at_ms > STALE_AFTER_MS) {
        return false;
    }
    *x10 = s.x10;
    return true;
}

bool sensors_get_control(sensor_sel_e sel, int16_t *x10)
{
    if (sel < SENSOR_SLOTS) {
        return sensors_get((uint8_t)sel, x10);
    }

    int     n   = 0;
    int32_t sum = 0;
    int16_t min = INT16_MAX;
    for (uint8_t i = 0; i < SENSOR_SLOTS; i++) {
        int16_t v;
        if (sensors_get(i, &v)) {
            n++;
            sum += v;
            if (v < min) {
                min = v;
            }
        }
    }
    if (n == 0) {
        return false;
    }
    *x10 = sel == SENSOR_SEL_MIN ? min : (int16_t)(sum / n);
    return true;
}
