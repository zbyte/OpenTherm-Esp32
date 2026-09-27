/**
 * @file bmp180.h
 * @brief On-board BMP180 (GY-68): temperature -> sensor slot 3, pressure.
 *
 * The board temperature reads a few degrees high (the ESP32 warms it), so it
 * is reported but not the default control sensor.
 */

#ifndef BMP180_H_
#define BMP180_H_

#include <stdbool.h>
#include <stdint.h>

#define BMP180_SLOT 2 /* sensor slot index (shown as sensor 3) */

void bmp180_init(void);

/* Last pressure in Pa; false if the sensor has not answered. */
bool bmp180_pressure(int32_t *pa);

void bmp180_print(void);

#endif /* BMP180_H_ */
