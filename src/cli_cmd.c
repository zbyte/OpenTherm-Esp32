/**
 * @file cli_cmd.c
 * @brief Debug CLI over the USB serial console (prompt "tuya> ").
 *
 * The license commands (auth, auth-read, auth-reset) are registered by
 * tuya_authorize_init() in cloud.c.
 */

#include <string.h>

#include "tal_api.h"
#include "tal_cli.h"
#include "tuya_iot.h"

#include "cloud.h"
#include "heating_state.h"

extern void tal_kv_cmd(int argc, char *argv[]);
extern void netmgr_cmd(int argc, char *argv[]);

/* Simulates a change made on the device itself (later: button/regulator). */
static void cmd_heat(int argc, char *argv[])
{
    if (argc == 2 && 0 == strcmp(argv[1], "on")) {
        heating_set_switch(true, HEATING_SRC_LOCAL);
    } else if (argc == 2 && 0 == strcmp(argv[1], "off")) {
        heating_set_switch(false, HEATING_SRC_LOCAL);
    } else {
        PR_INFO("usage: heat <on|off>");
    }
}

static void cmd_status(int argc, char *argv[])
{
    PR_NOTICE("switch=%d online=%d activated=%d heap=%d", heating_get_switch(), cloud_is_online(),
              tuya_iot_activated(tuya_iot_client_get()), tal_system_get_free_heap_size());
}

/* Unbind from the app and restart pairing; heating settings are kept. */
static void cmd_unbind(int argc, char *argv[])
{
    tuya_iot_reset(tuya_iot_client_get());
}

static cli_cmd_t s_cli_cmd[] = {
    {.name = "heat", .func = cmd_heat, .help = "heat <on|off>: local switch change"},
    {.name = "status", .func = cmd_status, .help = "show heating/cloud status"},
    {.name = "unbind", .func = cmd_unbind, .help = "unbind and restart pairing"},
    {.name = "kv", .func = tal_kv_cmd, .help = "kv store"},
    {.name = "netmgr", .func = netmgr_cmd, .help = "network manager"},
};

void app_cli_init(void)
{
    tal_cli_cmd_register(s_cli_cmd, sizeof(s_cli_cmd) / sizeof(s_cli_cmd[0]));
}
