/**
 * @file energy.h
 * @brief Estimated boiler power and energy from OpenTherm data.
 *
 * power = relative modulation (ID17) x rated power; without modulation data
 * "elements on" counts as full power (an upper estimate). The energy total
 * survives resets (RTC memory) and power loss (KV, saved every 15 min).
 */

#ifndef ENERGY_H_
#define ENERGY_H_

#include <stdbool.h>
#include <stdint.h>

#include "boiler.h"

void energy_init(void);

/* Called by the regulator every step with the latest boiler status. */
void energy_tick(float dt_s, const boiler_status_t *b);

uint32_t energy_power_w(void);
uint32_t energy_total_centi_kwh(void); /* 0.01 kWh units */

/* CLI: reset the counter (e.g. after installation). */
void energy_reset(void);

#endif /* ENERGY_H_ */
