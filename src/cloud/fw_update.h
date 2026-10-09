/**
 * @file fw_update.h
 * @brief Firmware update from ThingsBoard (OTA packages, MQTT API).
 *
 * ThingsBoard announces an assigned package with the shared attributes
 * fw_title, fw_version, fw_size, fw_checksum and fw_checksum_algorithm. If
 * the title is this firmware's name and the version differs from the running
 * one, the board downloads the image in chunks (v2/fw/request|response),
 * resuming across reconnects, checks the checksum and restarts into it. The
 * new image confirms itself once online, otherwise the bootloader rolls back
 * and that version is not tried again. Progress goes out as telemetry:
 * fw_state, fw_error, fw_progress, current_fw_title, current_fw_version.
 *
 * Called by the cloud client: messages from the MQTT task, tick from the
 * cloud task.
 */

#ifndef FW_UPDATE_H_
#define FW_UPDATE_H_

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

/* Shared keys to request on connect (comma-separated, no spaces). */
#define FW_UPDATE_SHARED_KEYS "fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm"

void fw_update_init(void);
/* Subscribes and resumes a download; returns true while one is running (the
 * caller then skips its full resend: reconnects may come every few seconds). */
bool fw_update_on_connect(void);
/* Shared attributes: a push ({"fw_version":..} or {"deleted":[..]}) or the
 * "shared" object of the response to the request made on connect. */
void fw_update_on_shared(const cJSON *obj);
/* true if the message was a firmware chunk */
bool fw_update_on_message(const char *topic, int topic_len, const char *data, int data_len);
void fw_update_tick(void);
/* One line for the console. */
void fw_update_describe(char *buf, size_t cap);

#endif /* FW_UPDATE_H_ */
