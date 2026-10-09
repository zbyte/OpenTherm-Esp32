/**
 * @file cloud.h
 * @brief ThingsBoard client over MQTT: telemetry, attributes, RPC commands.
 */

#ifndef CLOUD_H_
#define CLOUD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Loads the server settings; call once from the app thread. */
void cloud_init(void);

/* Runs the client loop (connect, reports); never returns. */
void cloud_run(void);

/* Report hooks, any thread (sent when online). Masks: SET_F_* and REG_F_*. */
void cloud_report_settings(uint32_t fields);
void cloud_report_status(uint32_t fields);
void cloud_report_all(void);

bool cloud_is_online(void);

typedef enum {
    CLOUD_SETUP,   /* setup access point is on, no uplink */
    CLOUD_OFFLINE, /* no network or no broker connection */
    CLOUD_ONLINE,
} cloud_state_e;

cloud_state_e cloud_state(void);

/* Server settings, persisted; the client reconnects with them. */
bool cloud_set_server(const char *host, uint16_t port);
bool cloud_set_token(const char *token);
void cloud_get_server(char *host, size_t cap, uint16_t *port, bool *has_token);

#endif /* CLOUD_H_ */
