/**
 * @file clock.h
 * @brief Local wall clock for the schedule.
 *
 * Time comes from the Tuya cloud once online (with the user's time zone), or
 * from the CLI for bench tests. Unknown time means the schedule is not run.
 */

#ifndef CLOCK_H_
#define CLOCK_H_

#include <stdbool.h>
#include <stdint.h>

void clock_init(void);

/* Local weekday (0 = Monday) and minute of day; false if time is unknown. */
bool clock_now(uint8_t *weekday, uint16_t *minute);

/* Sets UTC time (seconds since epoch) manually. */
void clock_set_utc(uint32_t epoch);

#endif /* CLOCK_H_ */
