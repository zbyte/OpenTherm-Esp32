/**
 * @file sensors.h
 * @brief Room temperature sensor slots and control-sensor selection.
 *
 * Drivers (DS18B20, SHT4x, BLE, emulator, CLI) push readings into slots; a
 * slot is valid while its last reading is fresh.
 */

#ifndef SENSORS_H_
#define SENSORS_H_

#include <stdbool.h>
#include <stdint.h>

#define SENSOR_SLOTS 4

typedef enum {
    SENSOR_SEL_ROOM1 = 0, /* ..SENSOR_SEL_ROOM4 = 3: a specific slot */
    SENSOR_SEL_MIN   = 4,
    SENSOR_SEL_AVG   = 5,
} sensor_sel_e;

void sensors_init(void);
void sensors_set(uint8_t slot, int16_t x10);
bool sensors_get(uint8_t slot, int16_t *x10);

/* Temperature used by the regulator; false when no valid source. */
bool sensors_get_control(sensor_sel_e sel, int16_t *x10);

#endif /* SENSORS_H_ */
