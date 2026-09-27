/**
 * @file supervisor.h
 * @brief Watchdog supervisor: the hardware task watchdog is fed only while
 *        every control task keeps beating; also confirms a fresh OTA image.
 */

#ifndef SUPERVISOR_H_
#define SUPERVISOR_H_

typedef enum {
    SUP_BOILER,
    SUP_REGULATOR,
    SUP_COUNT
} sup_task_e;

void supervisor_init(void);
void supervisor_beat(sup_task_e task);

/* Which task stalled before the last watchdog reset, or "none". */
const char *supervisor_last_stall(void);

/* Test hook (CLI): ignore beats from `task` so the watchdog fires. */
void supervisor_simulate_stall(sup_task_e task);

#endif /* SUPERVISOR_H_ */
