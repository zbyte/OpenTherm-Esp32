/**
 * @file schedule.h
 * @brief Weekly program.
 *
 * 7 days (Monday first) x 4 switch points x 4 bytes:
 * hour, minute, temperature (big-endian, 0.1 degC). The temperature is the
 * boiler FLOW setpoint (below 20 degC = boiler off) and holds until the next
 * point, across midnight. Stored as is in the settings (Sunday = day 7).
 */

#ifndef SCHEDULE_H_
#define SCHEDULE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCHED_DAYS   7
#define SCHED_POINTS 4
#define SCHED_BYTES  (SCHED_DAYS * SCHED_POINTS * 4)

void schedule_default(uint8_t *s);
bool schedule_validate(const uint8_t *s, size_t len);

/* Target for local weekday (0 = Monday) and minute of day. */
bool schedule_target(const uint8_t *s, uint8_t weekday, uint16_t minute, int16_t *x10);

void schedule_get_point(const uint8_t *s, uint8_t day, uint8_t idx, uint8_t *h, uint8_t *m, int16_t *x10);
void schedule_set_point(uint8_t *s, uint8_t day, uint8_t idx, uint8_t h, uint8_t m, int16_t x10);

#endif /* SCHEDULE_H_ */
