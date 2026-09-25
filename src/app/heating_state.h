/**
 * @file heating_state.h
 * @brief Heating settings owned by the device, persisted in KV.
 *
 * The device is the source of truth: settings survive reboots, loss of the
 * cloud and unbinding from the app. Setters persist and fire the change
 * callback so the cloud layer can report the new value.
 */

#ifndef HEATING_STATE_H_
#define HEATING_STATE_H_

#include <stdbool.h>

typedef enum {
    HEATING_SRC_LOCAL, /* changed on the device (button, CLI, regulator) */
    HEATING_SRC_CLOUD, /* changed by a DP command from the app/cloud */
} heating_src_e;

typedef void (*heating_change_cb_t)(heating_src_e src);

void heating_state_init(heating_change_cb_t on_change);

bool heating_get_switch(void);
void heating_set_switch(bool on, heating_src_e src);

#endif /* HEATING_STATE_H_ */
