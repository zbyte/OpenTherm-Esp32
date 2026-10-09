/**
 * @file settings.c
 * @brief User settings owned by the device, persisted in KV.
 */

#include "settings.h"

#include <stddef.h>
#include <string.h>

#include "tal_api.h"
#include "tuya_kconfig.h"

#include "sensors.h"

#define KV_KEY          "heat_cfg"
#define SETTINGS_FORMAT 3 /* 2: + boiler_set_x10, 3: + tz_min */

/* Room setpoint limits (ranges kept from the original product model). */
#define UPPER_MIN 250
#define UPPER_MAX 350
#define LOWER_MIN 50
#define LOWER_MAX 150

typedef struct {
    uint8_t    format;
    settings_t s;
} stored_t;

static MUTEX_HANDLE         s_lock;
static settings_t           s_cfg;
static settings_change_cb_t s_on_change;

static void set_defaults(settings_t *c)
{
    memset(c, 0, sizeof(*c));
    /* Out of the box the device heats with defaults (spec). */
    c->sw             = true;
    c->mode           = HEAT_MODE_HOME; /* room at temp_set */
    c->temp_set_x10   = HEAT_DEFAULT_SET_X10;
    c->upper_x10      = UPPER_MAX;
    c->lower_x10      = LOWER_MIN;
    c->control_sensor = SENSOR_SEL_ROOM1;
    schedule_default(c->schedule);
    c->boiler_set_x10 = 500;
    c->tz_min         = HEAT_DEFAULT_TZ_MIN;
}

static void save_locked(void)
{
    stored_t st = {.format = SETTINGS_FORMAT, .s = s_cfg};
    if (OPRT_OK != tal_kv_set(KV_KEY, (const uint8_t *)&st, sizeof(st))) {
        PR_ERR("settings: save failed");
    }
}

static void load(void)
{
    uint8_t *buf = NULL;
    size_t   len = 0;

    set_defaults(&s_cfg);
    if (OPRT_OK == tal_kv_get(KV_KEY, &buf, &len)) {
        const stored_t *st   = (const stored_t *)buf;
        size_t          body = len > offsetof(stored_t, s) ? len - offsetof(stored_t, s) : 0;
        if (st->format >= 1 && st->format <= SETTINGS_FORMAT && body > 0 && body <= sizeof(settings_t)) {
            /* older formats are a prefix: newer fields keep their defaults */
            memcpy(&s_cfg, &st->s, body);
            if (st->format != SETTINGS_FORMAT) {
                PR_NOTICE("settings: migrated from format %d", st->format);
            }
        } else {
            PR_WARN("settings: stored format %d/%u bytes not understood, using defaults", st->format,
                    (unsigned)len);
        }
    }
    if (buf) {
        tal_kv_free(buf);
    }
}

static void notify(uint32_t fields, settings_src_e src)
{
    /* Commands from the cloud are echoed even when nothing changed. */
    if (s_on_change && (fields || src == SETTINGS_SRC_CLOUD)) {
        s_on_change(fields, src);
    }
}

void settings_init(settings_change_cb_t on_change)
{
    tal_mutex_create_init(&s_lock);
    s_on_change = on_change;
    load();
    PR_NOTICE("settings: sw=%d mode=%d set=%d range=%d..%d sensor=%d", s_cfg.sw, s_cfg.mode, s_cfg.temp_set_x10,
              s_cfg.lower_x10, s_cfg.upper_x10, s_cfg.control_sensor);
}

void settings_get(settings_t *out)
{
    tal_mutex_lock(s_lock);
    *out = s_cfg;
    tal_mutex_unlock(s_lock);
}

/* Applies `apply` under the lock, saves on change, then notifies. */
#define UPDATE(field_mask, cond, stmt)                                                                                 \
    do {                                                                                                               \
        uint32_t changed = 0;                                                                                          \
        tal_mutex_lock(s_lock);                                                                                        \
        if (cond) {                                                                                                    \
            stmt;                                                                                                      \
            changed = (field_mask);                                                                                    \
            save_locked();                                                                                             \
        }                                                                                                              \
        tal_mutex_unlock(s_lock);                                                                                      \
        notify(changed, src);                                                                                          \
    } while (0)

void settings_set_switch(bool on, settings_src_e src)
{
    UPDATE(SET_F_SWITCH, s_cfg.sw != on, s_cfg.sw = on);
    PR_NOTICE("settings: switch=%d", on);
}

bool settings_set_mode(uint8_t mode, settings_src_e src)
{
    if (mode >= HEAT_MODE_COUNT) {
        return false;
    }
    UPDATE(SET_F_MODE, s_cfg.mode != mode, s_cfg.mode = mode);
    PR_NOTICE("settings: mode=%d", mode);
    return true;
}

void settings_set_temp_set(int16_t x10, settings_src_e src)
{
    tal_mutex_lock(s_lock);
    int16_t lo = s_cfg.lower_x10, hi = s_cfg.upper_x10;
    tal_mutex_unlock(s_lock);
    if (x10 < lo) {
        x10 = lo;
    }
    if (x10 > hi) {
        x10 = hi;
    }
    UPDATE(SET_F_TEMP_SET, s_cfg.temp_set_x10 != x10, s_cfg.temp_set_x10 = x10);
    PR_NOTICE("settings: temp_set=%d", x10);
}

bool settings_set_upper(int16_t x10, settings_src_e src)
{
    if (x10 < UPPER_MIN || x10 > UPPER_MAX) {
        return false;
    }
    UPDATE(SET_F_UPPER, s_cfg.upper_x10 != x10, s_cfg.upper_x10 = x10);
    settings_set_temp_set(s_cfg.temp_set_x10, SETTINGS_SRC_LOCAL); /* re-clamp */
    return true;
}

bool settings_set_lower(int16_t x10, settings_src_e src)
{
    if (x10 < LOWER_MIN || x10 > LOWER_MAX) {
        return false;
    }
    UPDATE(SET_F_LOWER, s_cfg.lower_x10 != x10, s_cfg.lower_x10 = x10);
    settings_set_temp_set(s_cfg.temp_set_x10, SETTINGS_SRC_LOCAL);
    return true;
}

bool settings_set_schedule(const uint8_t *data, size_t len, settings_src_e src)
{
    if (!schedule_validate(data, len)) {
        PR_WARN("settings: schedule rejected (len %u)", (unsigned)len);
        return false;
    }
    UPDATE(SET_F_SCHEDULE, memcmp(s_cfg.schedule, data, SCHED_BYTES) != 0,
           memcpy(s_cfg.schedule, data, SCHED_BYTES));
    PR_NOTICE("settings: schedule updated");
    return true;
}

bool settings_set_control_sensor(uint8_t sel, settings_src_e src)
{
    if (sel > SENSOR_SEL_AVG) {
        return false;
    }
    UPDATE(SET_F_SENSOR, s_cfg.control_sensor != sel, s_cfg.control_sensor = sel);
    return true;
}

bool settings_set_boiler_set(int16_t x10, settings_src_e src)
{
    if (x10 < BOILER_SET_MIN_X10 || x10 > BOILER_SET_MAX_X10) {
        return false;
    }
    UPDATE(SET_F_BOILER_SET, s_cfg.boiler_set_x10 != x10, s_cfg.boiler_set_x10 = x10);
    PR_NOTICE("settings: boiler_set=%d", x10);
    return true;
}

bool settings_set_tz(int16_t minutes, settings_src_e src)
{
    if (minutes < -720 || minutes > 840) {
        return false;
    }
    UPDATE(SET_F_TZ, s_cfg.tz_min != minutes, s_cfg.tz_min = minutes);
    PR_NOTICE("settings: tz=%d min", minutes);
    return true;
}

void settings_reset(void)
{
    settings_src_e src = SETTINGS_SRC_LOCAL;
    UPDATE(SET_F_ALL, true, set_defaults(&s_cfg));
    PR_NOTICE("settings: reset to defaults");
}
