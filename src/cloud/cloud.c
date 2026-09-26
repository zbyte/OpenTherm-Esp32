/**
 * @file cloud.c
 * @brief Tuya IoT client: activation, events and DP exchange.
 *
 * Based on apps/tuya_cloud/switch_demo from TuyaOpen. The cloud is a remote
 * control only: DP commands go to settings, and every change of settings or
 * regulator status (from any source) is reported back.
 */

#include "cloud.h"

#include <stdlib.h>
#include <string.h>

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
#include "regulator.h"
#include "settings.h"

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

#define MAX_DPS 8

static tuya_iot_client_t  s_client;
static tuya_iot_license_t s_license;

bool cloud_is_online(void)
{
    return tuya_iot_is_connected();
}

cloud_state_e cloud_state(void)
{
    if (!tuya_iot_activated(&s_client)) {
        return CLOUD_PAIRING;
    }
    return cloud_is_online() ? CLOUD_ONLINE : CLOUD_OFFLINE;
}

static void report_objs(dp_obj_t *dps, uint16_t n)
{
    if (n && cloud_is_online()) {
        tuya_iot_dp_obj_report(&s_client, NULL, dps, n, 0);
    }
}

static void report_schedule(const uint8_t *sched)
{
    if (!cloud_is_online()) {
        return;
    }
    dp_raw_t *dp = malloc(sizeof(dp_raw_t) + SCHED_BYTES);
    if (!dp) {
        return;
    }
    dp->id  = DP_ID_WEEK_PROGRAM;
    dp->len = SCHED_BYTES;
    memcpy(dp->data, sched, SCHED_BYTES);
    tuya_iot_dp_raw_report(&s_client, NULL, dp, 3);
    free(dp);
}

void cloud_report_settings(uint32_t fields)
{
    settings_t c;
    settings_get(&c);

    dp_obj_t dps[MAX_DPS];
    uint16_t n = 0;
    if (fields & SET_F_SWITCH) {
        dps[n++] = (dp_obj_t){.id = DP_ID_SWITCH, .type = PROP_BOOL, .value.dp_bool = c.sw};
    }
    if (fields & SET_F_MODE) {
        dps[n++] = (dp_obj_t){.id = DP_ID_MODE, .type = PROP_ENUM, .value.dp_enum = c.mode};
    }
    if (fields & SET_F_TEMP_SET) {
        dps[n++] = (dp_obj_t){.id = DP_ID_TEMP_SET, .type = PROP_VALUE, .value.dp_value = c.temp_set_x10};
    }
    if (fields & SET_F_UPPER) {
        dps[n++] = (dp_obj_t){.id = DP_ID_UPPER_TEMP, .type = PROP_VALUE, .value.dp_value = c.upper_x10};
    }
    if (fields & SET_F_LOWER) {
        dps[n++] = (dp_obj_t){.id = DP_ID_LOWER_TEMP, .type = PROP_VALUE, .value.dp_value = c.lower_x10};
    }
    report_objs(dps, n);

    if (fields & SET_F_SCHEDULE) {
        report_schedule(c.schedule);
    }
}

void cloud_report_status(uint32_t fields)
{
    regulator_status_t st;
    regulator_get_status(&st);

    dp_obj_t dps[MAX_DPS];
    uint16_t n = 0;
    if ((fields & REG_F_ROOM) && st.sensor_ok) {
        /* temp_current has scale 0: whole degrees, rounded */
        int v    = st.room_x10 >= 0 ? (st.room_x10 + 5) / 10 : (st.room_x10 - 5) / 10;
        dps[n++] = (dp_obj_t){.id = DP_ID_TEMP_CURRENT, .type = PROP_VALUE, .value.dp_value = v};
    }
    if (fields & REG_F_STATE) {
        dps[n++] = (dp_obj_t){.id = DP_ID_WORK_STATE, .type = PROP_ENUM, .value.dp_enum = st.work_state};
    }
    if (fields & REG_F_FAULTS) {
        dps[n++] = (dp_obj_t){.id = DP_ID_FAULT, .type = PROP_BITMAP, .value.dp_bitmap = st.faults};
    }
    report_objs(dps, n);
}

void cloud_report_all(void)
{
    cloud_report_settings(0xffffffff);
    cloud_report_status(0xffffffff);
}

static void handle_dp_obj(dp_obj_recv_t *dpobj)
{
    for (uint32_t i = 0; i < dpobj->dpscnt; i++) {
        dp_obj_t *dp = dpobj->dps + i;
        PR_DEBUG("dp rx id:%d type:%d", dp->id, dp->type);

        switch (dp->id) {
        case DP_ID_SWITCH:
            settings_set_switch(dp->value.dp_bool, SETTINGS_SRC_CLOUD);
            break;
        case DP_ID_MODE:
            if (!settings_set_mode((uint8_t)dp->value.dp_enum, SETTINGS_SRC_CLOUD)) {
                cloud_report_settings(SET_F_MODE); /* bounce back the real value */
            }
            break;
        case DP_ID_TEMP_SET:
            settings_set_temp_set((int16_t)dp->value.dp_value, SETTINGS_SRC_CLOUD);
            break;
        case DP_ID_UPPER_TEMP:
            if (!settings_set_upper((int16_t)dp->value.dp_value, SETTINGS_SRC_CLOUD)) {
                cloud_report_settings(SET_F_UPPER);
            }
            break;
        case DP_ID_LOWER_TEMP:
            if (!settings_set_lower((int16_t)dp->value.dp_value, SETTINGS_SRC_CLOUD)) {
                cloud_report_settings(SET_F_LOWER);
            }
            break;
        default:
            PR_WARN("dp %d not handled", dp->id);
            break;
        }
    }
}

static void handle_dp_raw(dp_raw_recv_t *dpraw)
{
    dp_raw_t *dp = &dpraw->dp;

    /* Keep the raw dump: it is how the schedule layout gets verified. */
    PR_NOTICE("dp rx id:%d type:RAW len:%d", dp->id, dp->len);
    for (uint32_t i = 0; i < dp->len; i++) {
        PR_DEBUG_RAW("%02x%s", dp->data[i], (i % 4 == 3) ? " " : "");
    }
    PR_DEBUG_RAW("\r\n");

    if (dp->id == DP_ID_WEEK_PROGRAM) {
        if (!settings_set_schedule(dp->data, dp->len, SETTINGS_SRC_CLOUD)) {
            cloud_report_settings(SET_F_SCHEDULE); /* bounce back what we run */
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

    case TUYA_EVENT_TIMESTAMP_SYNC:
        PR_NOTICE("time synced from cloud");
        break;

    case TUYA_EVENT_RESET:
        /* Unbinding only drops activation data inside the SDK. Heating settings
         * live under our own KV key and are deliberately kept (spec). */
        PR_NOTICE("device reset/unbind, type:%d", event->value.asInteger);
        break;

    case TUYA_EVENT_RESET_COMPLETE:
        PR_NOTICE("reset complete, rebooting");
        tal_system_reset();
        break;

    case TUYA_EVENT_DP_RECEIVE_OBJ:
        handle_dp_obj(event->value.dpobj);
        break;

    case TUYA_EVENT_DP_RECEIVE_RAW:
        handle_dp_raw(event->value.dpraw);
        break;

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
