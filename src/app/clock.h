/**
 * @file clock.h
 * @brief Local wall clock for the schedule.
 *
 * UTC comes from SNTP once the network is up, or from the CLI for bench
 * tests; the time zone is a setting. Unknown time means the schedule is not
 * run.
 */

#ifndef CLOCK_H_
#define CLOCK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Call after settings_init(): applies the stored time zone. */
void clock_init(void);

/* Local weekday (0 = Monday) and minute of day; false if time is unknown. */
bool clock_now(uint8_t *weekday, uint16_t *minute);

/* Local time as "YYYY-MM-DD HH:MM:SS"; "unknown" and false if not set. */
bool clock_format(char *buf, size_t len);

/* Seconds since the last SNTP answer; false if there was none yet. */
bool clock_sync_age(uint32_t *seconds);

/* Sets UTC time (seconds since epoch) manually. */
void clock_set_utc(uint32_t epoch);

/* UTC time from a trusted source (SNTP). */
void clock_set_utc_synced(uint32_t epoch);

/* Applies the time zone setting (offset from UTC, minutes). */
void clock_set_tz(int16_t minutes);

#endif /* CLOCK_H_ */
