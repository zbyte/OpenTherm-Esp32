/**
 * @file cloud.h
 * @brief Tuya IoT client: activation, events and DP exchange.
 */

#ifndef CLOUD_H_
#define CLOUD_H_

#include <stdbool.h>

/* Initializes the client and network manager; call once from the app thread. */
void cloud_init(void);

/* Runs the Tuya client loop; never returns. */
void cloud_run(void);

/* Reports all device-owned DPs (no-op while offline). */
void cloud_report_all(void);

bool cloud_is_online(void);

#endif /* CLOUD_H_ */
