/**
 * @file net.c
 * @brief Network link: Wi-Fi station with reconnects, setup access point.
 *
 * TuyaOpen's Wi-Fi layer runs either station or access point, not both, so
 * setup mode drops the station link until it ends. All Wi-Fi calls are made
 * from the net task; the event callback and the public setters only set flags.
 */

#include "net.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "tal_api.h"
#include "tal_wifi.h"

#include "clock.h"
#include "ot_port.h"

#if __has_include("ota_secret.h")
#include "ota_secret.h" /* WEB_PASSWORD doubles as the setup AP password */
#endif

#define KV_KEY        "netinfo"
#define TICK_MS       500
#define CONNECT_MS    30000 /* no answer from the driver: try again */
#define SETUP_MS      (10 * 60 * 1000)
#define BACKOFF_MIN_MS 5000
#define BACKOFF_MAX_MS 60000
#define SETUP_IP      "192.168.4.1"
#define NTP_SERVER    "pool.ntp.org"
#define CORE          0
#define PRIO          5

typedef enum {
    ST_IDLE,       /* no credentials */
    ST_CONNECTING, /* waiting for the driver */
    ST_UP,
    ST_DOWN,       /* waiting to retry */
    ST_SETUP,
} state_e;

static volatile state_e s_state;
static char             s_ssid[33];
static char             s_pass[65];
static MUTEX_HANDLE     s_lock;

/* requests to the net task */
static volatile bool s_req_setup;
static volatile bool s_req_reconnect;

/* driver events, consumed by the task. Stopping a connected station for a
 * new attempt reports "lost", so only "failed" ends an attempt early. */
static volatile bool s_ev_up;
static volatile bool s_ev_fail;
static volatile bool s_ev_lost;

static bool load_creds(void)
{
    uint8_t *buf = NULL;
    size_t   len = 0;
    bool     ok  = false;
    if (tal_kv_get(KV_KEY, &buf, &len) != OPRT_OK) {
        return false;
    }
    char *json = tal_malloc(len + 1);
    if (json) {
        memcpy(json, buf, len);
        json[len] = 0;
        cJSON *root = cJSON_Parse(json);
        cJSON *s = cJSON_GetObjectItem(root, "s"), *p = cJSON_GetObjectItem(root, "p");
        if (cJSON_IsString(s) && s->valuestring[0] && cJSON_IsString(p)) {
            snprintf(s_ssid, sizeof(s_ssid), "%s", s->valuestring);
            snprintf(s_pass, sizeof(s_pass), "%s", p->valuestring);
            ok = true;
        }
        cJSON_Delete(root);
        tal_free(json);
    }
    tal_kv_free(buf);
    return ok;
}

static void on_wifi_event(WF_EVENT_E event, void *arg)
{
    if (event == WFE_CONNECTED) {
        s_ev_up = true;
    } else if (event == WFE_CONNECT_FAILED) {
        s_ev_fail = true;
    } else if (event == WFE_DISCONNECTED) {
        s_ev_lost = true;
    }
}

static void connect_now(void)
{
    char ssid[33], pass[65];
    tal_mutex_lock(s_lock);
    memcpy(ssid, s_ssid, sizeof(ssid));
    memcpy(pass, s_pass, sizeof(pass));
    tal_mutex_unlock(s_lock);

    s_ev_up = s_ev_fail = s_ev_lost = false;
    PR_NOTICE("net: connecting to \"%s\"", ssid);
    tal_wifi_station_connect((int8_t *)ssid, (int8_t *)pass);
    s_state = ST_CONNECTING;
}

static void start_setup(void)
{
    NW_MAC_S mac = {0};
    tal_wifi_get_mac(WF_STATION, &mac);

    WF_AP_CFG_IF_S ap = {0};
    ap.s_len = (uint8_t)snprintf((char *)ap.ssid, sizeof(ap.ssid), "OpenTherm-%02X%02X", mac.mac[4], mac.mac[5]);
#ifdef WEB_PASSWORD
    ap.p_len = (uint8_t)snprintf((char *)ap.passwd, sizeof(ap.passwd), "%s", WEB_PASSWORD);
    ap.md    = WAAM_WPA2_PSK;
#else
    ap.md = WAAM_OPEN;
#endif
    ap.chan        = 6;
    ap.max_conn    = 2;
    ap.ms_interval = 100;
    strcpy(ap.ip.ip, SETUP_IP);
    strcpy(ap.ip.mask, "255.255.255.0");
    strcpy(ap.ip.gw, SETUP_IP);

    tal_wifi_station_disconnect();
    if (tal_wifi_ap_start(&ap) == OPRT_OK) {
        PR_NOTICE("net: setup access point \"%s\", web UI http://%s:8080/", ap.ssid, SETUP_IP);
    } else {
        PR_ERR("net: setup access point failed");
    }
    s_state = ST_SETUP;
}

static void on_ntp(uint32_t epoch)
{
    static bool logged;
    clock_set_utc_synced(epoch);
    if (!logged) {
        logged = true;
        PR_NOTICE("net: time synced");
    }
}

static void net_task(void *arg)
{
    uint32_t since = tal_system_get_millisecond(); /* entered the current state */
    uint32_t backoff = BACKOFF_MIN_MS;

    if (s_ssid[0]) {
        connect_now();
    } else {
        PR_WARN("net: no Wi-Fi credentials");
        start_setup();
    }

    for (;;) {
        uint32_t now  = tal_system_get_millisecond();
        state_e  prev = s_state;

        if (s_req_setup) {
            s_req_setup = false;
            if (s_state != ST_SETUP) {
                start_setup();
            }
        } else if (s_req_reconnect) {
            s_req_reconnect = false;
            if (s_state == ST_SETUP) {
                tal_wifi_ap_stop();
            }
            backoff = BACKOFF_MIN_MS;
            connect_now();
        }

        switch (s_state) {
        case ST_CONNECTING:
        case ST_DOWN: /* a late success still counts */
            if (s_ev_up) {
                s_ev_up = false;
                s_state = ST_UP;
                backoff = BACKOFF_MIN_MS;
                char ip[16] = "?";
                sys_port_sta_ip(ip, sizeof(ip));
                PR_NOTICE("net: up, ip %s", ip);
                sys_port_wifi_ps_off();
                sntp_port_start(NTP_SERVER, on_ntp);
            } else if (s_state == ST_CONNECTING && (s_ev_fail || now - since > CONNECT_MS)) {
                PR_WARN("net: connect failed, retry in %u s", (unsigned)(backoff / 1000));
                s_state = ST_DOWN;
            } else if (s_state == ST_DOWN && now - since >= backoff) {
                backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
                connect_now();
            }
            break;
        case ST_UP:
            if (s_ev_lost || s_ev_fail) {
                PR_WARN("net: link lost");
                s_state = ST_DOWN;
                backoff = BACKOFF_MIN_MS;
            }
            break;
        case ST_SETUP:
            if (s_ssid[0] && now - since >= SETUP_MS) {
                PR_NOTICE("net: setup time over");
                tal_wifi_ap_stop();
                connect_now();
            }
            break;
        default:
            break;
        }
        if (s_state != prev) {
            since = now;
            if (s_state != ST_CONNECTING) {
                s_ev_fail = s_ev_lost = false;
            }
        }
        tal_system_sleep(TICK_MS);
    }
}

void net_init(void)
{
    tal_mutex_create_init(&s_lock);
    if (load_creds()) {
        PR_NOTICE("net: Wi-Fi \"%s\"", s_ssid);
    }
    if (tal_wifi_init(on_wifi_event) != OPRT_OK) {
        PR_ERR("net: Wi-Fi init failed");
        return;
    }
    if (ot_port_task_create(net_task, "net", 4096, PRIO, CORE, NULL) != OT_PORT_OK) {
        PR_ERR("net: task create failed");
    }
}

bool net_is_up(void)
{
    return s_state == ST_UP;
}

bool net_setup_active(void)
{
    return s_state == ST_SETUP;
}

void net_setup_start(void)
{
    s_req_setup = true;
}

bool net_set_wifi(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) >= sizeof(s_ssid) || !password || strlen(password) >= sizeof(s_pass)) {
        return false;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "s", ssid);
    cJSON_AddStringToObject(root, "p", password);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    bool ok = json && tal_kv_set(KV_KEY, (const uint8_t *)json, strlen(json)) == OPRT_OK;
    cJSON_free(json);
    if (!ok) {
        PR_ERR("net: saving Wi-Fi credentials failed");
        return false;
    }

    tal_mutex_lock(s_lock);
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", password);
    tal_mutex_unlock(s_lock);
    PR_NOTICE("net: Wi-Fi set to \"%s\"", ssid);
    s_req_reconnect = true;
    return true;
}

void net_get_ssid(char *out, size_t cap)
{
    tal_mutex_lock(s_lock);
    snprintf(out, cap, "%s", s_ssid);
    tal_mutex_unlock(s_lock);
}

bool net_rssi(int8_t *dbm)
{
    return s_state == ST_UP && tal_wifi_station_get_conn_ap_rssi(dbm) == OPRT_OK;
}
