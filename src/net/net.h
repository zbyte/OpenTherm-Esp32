/**
 * @file net.h
 * @brief Network link: Wi-Fi station with reconnects, setup access point.
 *
 * Up to two networks, KV "netinfo" and "netinfo2" ({"s":ssid,"p":password});
 * the board connects to whichever of them it finds. Without credentials, or
 * after a 5..10 s button press, the board opens the access point "OpenTherm-XXXX" (password = web password)
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

/* Saves network n (0 or 1) and reconnects to it (leaves setup mode), unless
 * the board is up on the other network. An empty ssid removes network 1. */
bool net_set_wifi(uint8_t n, const char *ssid, const char *password);

/* Name of network n ("" if not set). */
void net_get_wifi(uint8_t n, char *out, size_t cap);

/* Network in use or being tried ("" if none) and signal of the current link. */
void net_get_ssid(char *out, size_t cap);
bool net_rssi(int8_t *dbm);

#endif /* NET_H_ */
