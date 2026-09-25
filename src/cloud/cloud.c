/**
 * @file cloud.c
 * @brief Tuya IoT client: activation, events and DP exchange.
 *
 * Based on apps/tuya_cloud/switch_demo from TuyaOpen. The cloud is a remote
 * control only: DP commands are applied to heating_state, and every change of
 * heating_state (from any source) is reported back.
 */

#include "cloud.h"

#include "cJSON.h"
#include "netmgr.h"
#include "tal_api.h"
#include "tal_cli.h"
#include "tuya_authorize.h"
#include "tuya_config.h"
#include "tuya_iot.h"
#include "tuya_iot_dp.h"
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#endif

#include "dp_ids.h"
#include "heating_state.h"

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

static tuya_iot_client_t  s_client;
static tuya_iot_license_t s_license;

bool cloud_is_online(void)
{
    return tuya_iot_is_connected();
}

void cloud_report_all(void)
{
    if (!cloud_is_online()) {
        return;
    }

    dp_obj_t dps[] = {
        {.id = DP_ID_SWITCH, .type = PROP_BOOL, .value.dp_bool = heating_get_switch()},
    };
    tuya_iot_dp_obj_report(&s_client, NULL, dps, sizeof(dps) / sizeof(dps[0]), 0);
}

static void handle_dp_obj(dp_obj_recv_t *dpobj)
{
    for (uint32_t i = 0; i < dpobj->dpscnt; i++) {
        dp_obj_t *dp = dpobj->dps + i;
        PR_DEBUG("dp rx id:%d type:%d", dp->id, dp->type);

        switch (dp->id) {
        case DP_ID_SWITCH:
            if (dp->type == PROP_BOOL) {
                heating_set_switch(dp->value.dp_bool, HEATING_SRC_CLOUD);
            }
            break;
        default:
            PR_WARN("dp %d not handled", dp->id);
            break;
        }
    }
}

static void user_event_handler_on(tuya_iot_client_t *client, tuya_event_msg_t *event)
{
    PR_DEBUG("Tuya Event ID:%d(%s)", event->id, EVENT_ID2STR(event->id));

    switch (event->id) {
    case TUYA_EVENT_BIND_START:
        PR_INFO("waiting for pairing (BLE)");
        break;

    case TUYA_EVENT_MQTT_CONNECTED:
        PR_NOTICE("cloud online");
        cloud_report_all();
        break;

    case TUYA_EVENT_MQTT_DISCONNECT:
        PR_WARN("cloud offline, heating keeps running locally");
        break;

    case TUYA_EVENT_RESET:
        /* Unbinding only drops activation data inside the SDK. Heating settings
         * live under our own KV keys and are deliberately kept (spec). */
        PR_NOTICE("device reset/unbind, type:%d", event->value.asInteger);
        break;

    case TUYA_EVENT_RESET_COMPLETE:
        PR_NOTICE("reset complete, rebooting");
        tal_system_reset();
        break;

    case TUYA_EVENT_DP_RECEIVE_OBJ:
        handle_dp_obj(event->value.dpobj);
        break;

    case TUYA_EVENT_DP_RECEIVE_RAW: {
        dp_raw_t *dp = &event->value.dpraw->dp;
        PR_DEBUG("dp rx id:%d type:RAW len:%d", dp->id, dp->len);
        for (uint32_t i = 0; i < dp->len; i++) {
            PR_DEBUG_RAW("%02x", dp->data[i]);
        }
        PR_DEBUG_RAW("\r\n");
    } break;

    case TUYA_EVENT_UPGRADE_NOTIFY:
        PR_INFO("OTA notify received (not handled yet)");
        break;

    default:
        break;
    }
}

static bool user_network_check(void)
{
    netmgr_status_e status = NETMGR_LINK_DOWN;
    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &status);
    return status != NETMGR_LINK_DOWN;
}

void cloud_init(void)
{
    int rt;

    tuya_authorize_init();
    if (OPRT_OK != tuya_authorize_read(&s_license)) {
        s_license.uuid    = TUYA_OPENSDK_UUID;
        s_license.authkey = TUYA_OPENSDK_AUTHKEY;
        PR_WARN("no license in KV, using tuya_config.h values; write one with: auth <uuid> <authkey>");
    }

    rt = tuya_iot_init(&s_client, &(const tuya_iot_config_t){
                                      .software_ver  = PROJECT_VERSION,
                                      .productkey    = TUYA_PRODUCT_ID,
                                      .uuid          = s_license.uuid,
                                      .authkey       = s_license.authkey,
                                      .event_handler = user_event_handler_on,
                                      .network_check = user_network_check,
                                  });
    if (rt != OPRT_OK) {
        PR_ERR("tuya_iot_init failed: %d", rt);
        return;
    }

    netmgr_type_e type = 0;
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    type |= NETCONN_WIFI;
#endif
    netmgr_init(type);

#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG, &(netcfg_args_t){.type = NETCFG_TUYA_BLE});
#endif

    tuya_iot_start(&s_client);
}

void cloud_run(void)
{
    for (;;) {
        tuya_iot_yield(&s_client);
    }
}
