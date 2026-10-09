/**
 * @file net.h
 * @brief Network link: Wi-Fi station with reconnects, setup access point.
 *
 * Credentials live in KV "netinfo" ({"s":ssid,"p":password}). Without credentials, or after a 5..10 s button press, the
 * board opens the access point "OpenTherm-XXXX" (password = web password)
 * with the web UI at http://192.168.4.1:8080/ to enter them.
 */

#ifndef NET_H_
#define NET_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void net_init(void);

bool net_is_up(void);        /* station has an IP address */
bool net_setup_active(void); /* setup access point is on */

/* Opens the setup access point for 10 minutes (button). */
void net_setup_start(void);

/* Saves new credentials and reconnects (leaves setup mode). */
bool net_set_wifi(const char *ssid, const char *password);

/* Configured network name ("" if none) and signal of the current link. */
void net_get_ssid(char *out, size_t cap);
bool net_rssi(int8_t *dbm);

#endif /* NET_H_ */
