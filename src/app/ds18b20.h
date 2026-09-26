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

#endif /* DS18B20_H_ */
