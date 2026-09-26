/**
 * @file settings.h
 * @brief User settings owned by the device, persisted in KV.
 *
 * The device is the source of truth: settings survive reboots, loss of the
 * cloud and unbinding from the app. Every setter persists and fires the
 * change callback so the cloud layer can report back.
 */

#ifndef SETTINGS_H_
#define SETTINGS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "schedule.h"

typedef enum {
    HEAT_MODE_AUTO   = 0, /* weekly program */
    HEAT_MODE_MANUAL = 1, /* temp_set */
    HEAT_MODE_HOME   = 2, /* temp_set as well (app "home" preset) */
    HEAT_MODE_LEAVE  = 3, /* away temperature */
    HEAT_MODE_COUNT
} heat_mode_e;

typedef enum {
    SETTINGS_SRC_LOCAL, /* device: CLI, button, defaults */
    SETTINGS_SRC_CLOUD, /* DP command from the app */
} settings_src_e;

/* Changed-field mask passed to the callback. */
#define SET_F_SWITCH   (1u << 0)
#define SET_F_MODE     (1u << 1)
#define SET_F_TEMP_SET (1u << 2)
#define SET_F_UPPER    (1u << 3)
#define SET_F_LOWER    (1u << 4)
#define SET_F_SCHEDULE (1u << 5)
#define SET_F_SENSOR   (1u << 6)

typedef struct {
    bool    sw;
    uint8_t mode;
    int16_t temp_set_x10;
    int16_t upper_x10;
    int16_t lower_x10;
    uint8_t control_sensor; /* sensor_sel_e */
    uint8_t schedule[SCHED_BYTES];
} settings_t;

typedef void (*settings_change_cb_t)(uint32_t fields, settings_src_e src);

void settings_init(settings_change_cb_t on_change);
void settings_get(settings_t *out);

void settings_set_switch(bool on, settings_src_e src);
bool settings_set_mode(uint8_t mode, settings_src_e src);
void settings_set_temp_set(int16_t x10, settings_src_e src); /* clamped to lower..upper */
bool settings_set_upper(int16_t x10, settings_src_e src);
bool settings_set_lower(int16_t x10, settings_src_e src);
bool settings_set_schedule(const uint8_t *data, size_t len, settings_src_e src);
bool settings_set_control_sensor(uint8_t sel, settings_src_e src);

/* Restores defaults (keeps nothing); used by the long button press. */
void settings_reset(void);

#endif /* SETTINGS_H_ */
