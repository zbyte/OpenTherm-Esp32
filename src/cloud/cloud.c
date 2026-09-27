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
#include "dp_schema.h"
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#endif

#include "bmp180.h"
#include "dp_ids.h"
#include "sensors.h"
#include "ot_port.h"
#include "regulator.h"
#include "settings.h"

#if __has_include("ota_secret.h")
#include "ota_secret.h" /* defines OTA_TOKEN */
#endif
#define LAN_OTA_PORT 8080

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

static void do_report_settings(uint32_t fields)
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
    if (fields & SET_F_BOILER_SET) {
        /* scale 0: whole degrees */
        dps[n++] = (dp_obj_t){.id = DP_ID_BOILER_SET, .type = PROP_VALUE, .value.dp_value = c.boiler_set_x10 / 10};
    }
    report_objs(dps, n);

    if (fields & SET_F_SCHEDULE) {
        report_schedule(c.schedule);
    }
}

static void do_report_status(uint32_t fields)
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
    if ((fields & REG_F_FLOW) && st.flow_valid) {
        dps[n++] = (dp_obj_t){.id = DP_ID_FLOW_TEMP, .type = PROP_VALUE, .value.dp_value = (st.flow_x10 + 5) / 10};
    }
    if (fields & REG_F_STATE) {
        dps[n++] = (dp_obj_t){.id = DP_ID_WORK_STATE, .type = PROP_ENUM, .value.dp_enum = st.work_state};
    }
    report_objs(dps, n);

    if (fields & REG_F_FAULTS) {
        /* Not via dp_obj: TuyaOpen's dp_rept_valid_check() requires
         * type == PROP_BITMAP while dp_type_check() then requires
         * type == cJSON_Number, so an object bitmap report can never pass
         * ("bitmap check fail"). The JSON path validates it correctly. */
        char json[24];
        snprintf(json, sizeof(json), "{\"%d\":%u}", DP_ID_FAULT, (unsigned)st.faults);
        tuya_iot_dp_report_json(&s_client, json);
    }
}

/*
 * Reports are only queued here (any thread) and sent from the cloud thread
 * in cloud_run(). The SDK client is not thread-safe: reporting from the
 * regulator or CLI thread while the SDK was tearing down on an unbind from
 * the app could touch freed state.
 */
static volatile uint32_t s_pending_settings;
static volatile uint32_t s_pending_status;

void cloud_report_settings(uint32_t fields)
{
    __atomic_fetch_or(&s_pending_settings, fields, __ATOMIC_SEQ_CST);
}

void cloud_report_status(uint32_t fields)
{
    __atomic_fetch_or(&s_pending_status, fields, __ATOMIC_SEQ_CST);
}

void cloud_report_all(void)
{
    cloud_report_settings(0xffffffff);
    cloud_report_status(0xffffffff);
}

/* Individual sensors and pressure: sent when they move enough, and at least
 * every SENSOR_REFRESH_MS so the app never shows a stale value for long. */
#define SENSOR_CHECK_MS   10000
#define SENSOR_REFRESH_MS (5 * 60 * 1000)

static void report_sensors(void)
{
    static const uint8_t ids[3] = {DP_ID_TEMP_S1, DP_ID_TEMP_S2, DP_ID_TEMP_BOARD};
    static int32_t       last[4] = {INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN};
    static uint32_t      last_check, last_full;

    uint32_t now = tal_system_get_millisecond();
    if (now - last_check < SENSOR_CHECK_MS) {
        return;
    }
    last_check = now;
    bool full  = now - last_full >= SENSOR_REFRESH_MS;
    if (full) {
        last_full = now;
    }

    dp_obj_t dps[4];
    uint16_t n = 0;
    for (uint8_t i = 0; i < 3; i++) {
        int16_t v;
        if (sensors_get(i, &v) && (full || last[i] == INT32_MIN || abs(v - last[i]) >= 2)) {
            dps[n++] = (dp_obj_t){.id = ids[i], .type = PROP_VALUE, .value.dp_value = v};
            last[i]  = v;
        }
    }
    int32_t pa;
    if (bmp180_pressure(&pa)) {
        int32_t hpa = (pa + 50) / 100;
        if (full || last[3] == INT32_MIN || abs(hpa - last[3]) >= 1) {
            dps[n++] = (dp_obj_t){.id = DP_ID_PRESSURE, .type = PROP_VALUE, .value.dp_value = hpa};
            last[3]  = hpa;
        }
    }
    report_objs(dps, n);
}

/* Cloud thread only. Pending bits survive while offline. */
static void flush_reports(void)
{
    if (!cloud_is_online()) {
        return;
    }
    uint32_t s = __atomic_exchange_n(&s_pending_settings, 0, __ATOMIC_SEQ_CST);
    uint32_t r = __atomic_exchange_n(&s_pending_status, 0, __ATOMIC_SEQ_CST);
    if (s) {
        do_report_settings(s);
    }
    if (r) {
        do_report_status(r);
    }
    report_sensors();
}

/* LAN update server, started once the network is up. The token comes from
 * include/ota_secret.h, generated by build.ps1 and kept out of git. */
static void lan_ota_enable(void)
{
#ifdef OTA_TOKEN
    char ip[16] = "?";
    sys_port_sta_ip(ip, sizeof(ip));
    if (lan_ota_start(OTA_TOKEN, LAN_OTA_PORT, PROJECT_VERSION) == OT_PORT_OK) {
        PR_NOTICE("LAN OTA: http://%s:%d/ota", ip, LAN_OTA_PORT);
    } else {
        PR_ERR("LAN OTA: server start failed");
    }
#else
    PR_WARN("LAN OTA disabled: no include/ota_secret.h");
#endif
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
        case DP_ID_BOILER_SET:
            if (!settings_set_boiler_set((int16_t)(dp->value.dp_value * 10), SETTINGS_SRC_CLOUD)) {
                cloud_report_settings(SET_F_BOILER_SET);
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
        lan_ota_enable();
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

/*
 * The SDK stores the DP schema it got at activation and never refreshes it,
 * so DPs added on the platform later are rejected ("DP ID 101 Invalid")
 * until the device is paired again. Patch the stored schema with the custom
 * DPs (same definitions as the product model) and rebuild it.
 */
static const struct {
    uint8_t     id;
    const char *mode;
    int         min, max, scale;
} s_custom_dps[] = {
    {DP_ID_BOILER_SET, "rw", 20, 80, 0},     {DP_ID_FLOW_TEMP, "ro", 0, 100, 0},
    {DP_ID_TEMP_S1, "ro", -400, 1250, 1},    {DP_ID_TEMP_S2, "ro", -400, 1250, 1},
    {DP_ID_TEMP_BOARD, "ro", -400, 850, 1},  {DP_ID_PRESSURE, "ro", 300, 1100, 0},
};

static bool schema_has_dp(cJSON *arr, int id)
{
    cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        cJSON *jid = cJSON_GetObjectItem(it, "id");
        if (jid && ((cJSON_IsNumber(jid) && jid->valueint == id) || (cJSON_IsString(jid) && atoi(jid->valuestring) == id))) {
            return true;
        }
    }
    return false;
}

static void schema_add_custom_dps(void)
{
    if (!tuya_iot_activated(&s_client)) {
        return; /* a fresh activation brings the full schema */
    }
    const char *sid = s_client.activate.schemaId;
    uint8_t    *buf = NULL;
    size_t      len = 0;
    if (tal_kv_get(sid, &buf, &len) != OPRT_OK) {
        return;
    }
    char *json = tal_malloc(len + 1);
    if (json) {
        memcpy(json, buf, len);
        json[len] = '\0';
    }
    tal_kv_free(buf);
    cJSON *arr = json ? cJSON_Parse(json) : NULL;
    tal_free(json);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return;
    }

    int added = 0;
    for (size_t i = 0; i < sizeof(s_custom_dps) / sizeof(s_custom_dps[0]); i++) {
        if (schema_has_dp(arr, s_custom_dps[i].id)) {
            continue;
        }
        cJSON *dp   = cJSON_CreateObject();
        cJSON *prop = cJSON_CreateObject();
        cJSON_AddNumberToObject(dp, "id", s_custom_dps[i].id);
        cJSON_AddStringToObject(dp, "mode", s_custom_dps[i].mode);
        cJSON_AddStringToObject(dp, "type", "obj");
        cJSON_AddStringToObject(prop, "type", "value");
        cJSON_AddNumberToObject(prop, "min", s_custom_dps[i].min);
        cJSON_AddNumberToObject(prop, "max", s_custom_dps[i].max);
        cJSON_AddNumberToObject(prop, "scale", s_custom_dps[i].scale);
        cJSON_AddNumberToObject(prop, "step", 1);
        cJSON_AddItemToObject(dp, "property", prop);
        cJSON_AddItemToArray(arr, dp);
        added++;
    }

    if (added) {
        char *out = cJSON_PrintUnformatted(arr);
        if (out && tal_kv_set(sid, (const uint8_t *)out, strlen(out)) == OPRT_OK) {
            dp_schema_delete(s_client.activate.devid);
            s_client.schema = NULL;
            if (dp_schema_create(s_client.activate.devid, out, &s_client.schema) == OPRT_OK) {
                PR_NOTICE("schema: added %d custom DPs", added);
            } else {
                PR_ERR("schema: rebuild failed");
            }
        }
        tal_free(out);
    }
    cJSON_Delete(arr);
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
    schema_add_custom_dps();

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
        flush_reports();
    }
}
