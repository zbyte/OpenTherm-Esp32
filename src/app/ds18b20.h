/**
 * @file ds18b20.h
 * @brief DS18B20 room sensors, one per GPIO (no ROM search), into sensor slots.
 */

#ifndef DS18B20_H_
#define DS18B20_H_

#include <stdint.h>

/* Starts polling: DS18B20_1_GPIO -> slot 0, DS18B20_2_GPIO -> slot 1. */
void ds18b20_init(void);

/* Per-sensor read statistics for the CLI. */
void ds18b20_print(void);

/* Strong pull-up during conversion: auto (parasite sensors only), on, off.
 * Clears the statistics so the modes can be compared. */
typedef enum {
    DS18B20_PULL_AUTO,
    DS18B20_PULL_ON,
    DS18B20_PULL_OFF,
} ds18b20_pull_e;

void ds18b20_set_pull(ds18b20_pull_e mode);

#endif /* DS18B20_H_ */
