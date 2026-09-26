/**
 * @file cloud.h
 * @brief Tuya IoT client: activation, events and DP exchange.
 */

#ifndef CLOUD_H_
#define CLOUD_H_

#include <stdbool.h>
#include <stdint.h>

/* Initializes the client and network manager; call once from the app thread. */
void cloud_init(void);

/* Runs the Tuya client loop; never returns. */
void cloud_run(void);

/* Report hooks (no-ops while offline). Masks: SET_F_* and REG_F_*. */
void cloud_report_settings(uint32_t fields);
void cloud_report_status(uint32_t fields);
void cloud_report_all(void);

bool cloud_is_online(void);

#endif /* CLOUD_H_ */
