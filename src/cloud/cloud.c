/**
 * @file cloud.c
 * @brief ThingsBoard client over MQTT: telemetry, attributes, RPC commands.
 *
 * The device stays the source of truth. Measurements go out as telemetry
 * (time series), settings as client attributes. Commands from the dashboard
 * come two ways, both through the same setters as the web UI and the CLI:
 *   - RPC (only while the device is online);
 *   - shared attributes set_* (delivered on reconnect too). A shared value is
 *     applied when it differs from the last one applied, so a stale server
 *     value never overwrites a setting changed locally in the meantime.
 *
 * Traffic is kept low for a metered link: a telemetry key is sent when it
 * moves past its dead band, and all of them every TELE_FULL_MS.
 *
 * Topics (ThingsBoard device MQTT API, token as user name):
 *   v1/devices/me/telemetry            device -> server, {"key":value,...}
 *   v1/devices/me/attributes           device -> server, client attributes
 *   v1/devices/me/rpc/request/<id>     server -> device, {"method":..,"params":..}
 *   v1/devices/me/rpc/response/<id>    device -> server, the result
 */

#include "cloud.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "tal_api.h"
#include "tal_wifi.h"

#include "bmp180.h"
#include "boiler.h"
#include "clock.h"
#include "energy.h"
#include "fw_update.h"
#include "log_ring.h"
#include "net.h"
#include "ot_port.h"
#include "regulator.h"
#include "sensors.h"
#include "settings.h"

#if __has_include("tb_secret.h")
#include "tb_secret.h" /* TB_HOST, TB_PORT, TB_TOKEN: defaults, kept out of git */
#endif
#ifndef TB_HOST
#define TB_HOST ""
#endif
#ifndef TB_PORT
#define TB_PORT 8883
#endif
#ifndef TB_TOKEN
#define TB_TOKEN ""
#endif

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

#define KV_KEY        "tb_cfg"
#define KEEPALIVE_S   60 /* the home router drops idle TCP sessions in under 2 min */
#define RECONNECT_MS  15000
#define TICK_MS       200
#define TELE_CHECK_MS 2000
#define TELE_FULL_MS  (10 * 60 * 1000)
#define ENERGY_MIN_MS 60000
#define CLI_OUT_MAX   1500

#define TOPIC_TELE "v1/devices/me/telemetry"
#define TOPIC_ATTR "v1/devices/me/attributes"
#define TOPIC_RPC  "v1/devices/me/rpc/request/"
#define TOPIC_RESP "v1/devices/me/rpc/response/"
#define TOPIC_AREQ "v1/devices/me/attributes/request/1"
#define TOPIC_ARSP "v1/devices/me/attributes/response/"
#define KV_SEEN    "tb_seen" /* shared attribute values last applied */

extern int app_cli_exec(const char *line);

static const char *const s_modes[HEAT_MODE_COUNT] = {"auto", "manual", "home", "leave"};
static const char *const s_sensors[]              = {"1", "2", "3", "4", "min", "avg"};
static const char *const s_work[]                 = {"heating", "warm", "idle"};
static const char *const s_days[SCHED_DAYS]       = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

/* ------------------------------------------------------------ settings */

static struct {
    char     host[64];
    uint16_t port;
    char     token[64];
    char     pass[64]; /* MQTT password, own broker only */
} s_cfg;
static MUTEX_HANDLE  s_cfg_lock;
static volatile bool s_restart; /* server settings changed */

static void cfg_save(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "h", s_cfg.host);
    cJSON_AddNumberToObject(root, "p", s_cfg.port);
    cJSON_AddStringToObject(root, "t", s_cfg.token);
    cJSON_AddStringToObject(root, "w", s_cfg.pass);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json || tal_kv_set(KV_KEY, (const uint8_t *)json, strlen(json)) != OPRT_OK) {
        PR_ERR("cloud: saving server settings failed");
    }
    cJSON_free(json);
}

static void cfg_load(void)
{
    snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", TB_HOST);
    s_cfg.port = TB_PORT;
    snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", TB_TOKEN);

    uint8_t *buf = NULL;
    size_t   len = 0;
    if (tal_kv_get(KV_KEY, &buf, &len) != OPRT_OK) {
        return;
    }
    char *json = tal_malloc(len + 1);
    if (json) {
        memcpy(json, buf, len);
        json[len]   = 0;
        cJSON *root = cJSON_Parse(json);
        cJSON *h = cJSON_GetObjectItem(root, "h"), *p = cJSON_GetObjectItem(root, "p");
        cJSON *t = cJSON_GetObjectItem(root, "t"), *w = cJSON_GetObjectItem(root, "w");
        if (cJSON_IsString(h) && h->valuestring[0]) {
            snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", h->valuestring);
        }
        if (cJSON_IsNumber(p) && p->valueint > 0 && p->valueint < 65536) {
            s_cfg.port = (uint16_t)p->valueint;
        }
        if (cJSON_IsString(t) && t->valuestring[0]) {
            snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", t->valuestring);
        }
        if (cJSON_IsString(w)) {
            snprintf(s_cfg.pass, sizeof(s_cfg.pass), "%s", w->valuestring);
        }
        cJSON_Delete(root);
        tal_free(json);
    }
    tal_kv_free(buf);
}

bool cloud_set_server(const char *host, uint16_t port)
{
    if (!host || !host[0] || strlen(host) >= sizeof(s_cfg.host) || !port) {
        return false;
    }
    tal_mutex_lock(s_cfg_lock);
    snprintf(s_cfg.host, sizeof(s_cfg.host), "%s", host);
    s_cfg.port = port;
    cfg_save();
    tal_mutex_unlock(s_cfg_lock);
    PR_NOTICE("cloud: server %s:%u", host, port);
    s_restart = true;
    return true;
}

bool cloud_set_token(const char *token)
{
    if (!token || !token[0] || strlen(token) >= sizeof(s_cfg.token)) {
        return false;
    }
    tal_mutex_lock(s_cfg_lock);
    snprintf(s_cfg.token, sizeof(s_cfg.token), "%s", token);
    cfg_save();
    tal_mutex_unlock(s_cfg_lock);
    PR_NOTICE("cloud: access token updated");
    s_restart = true;
    return true;
}

bool cloud_set_password(const char *password)
{
    if (!password || strlen(password) >= sizeof(s_cfg.pass)) {
        return false;
    }
    tal_mutex_lock(s_cfg_lock);
    snprintf(s_cfg.pass, sizeof(s_cfg.pass), "%s", password);
    cfg_save();
    tal_mutex_unlock(s_cfg_lock);
    PR_NOTICE("cloud: MQTT password %s", password[0] ? "set" : "cleared");
    s_restart = true;
    return true;
}

bool cloud_has_password(void)
{
    return s_cfg.pass[0] != 0;
}

void cloud_reconnect(void)
{
    s_restart = true;
}

void cloud_get_server(char *host, size_t cap, uint16_t *port, bool *has_token)
{
    tal_mutex_lock(s_cfg_lock);
    snprintf(host, cap, "%s", s_cfg.host);
    *port      = s_cfg.port;
    *has_token = s_cfg.token[0] != 0;
    tal_mutex_unlock(s_cfg_lock);
}

/* ---------------------------------------------------------- JSON output */

typedef struct {
    char  *buf;
    size_t cap, len;
    int    items; /* in the current object, for commas */
} jout_t;

static void jput(jout_t *o, const char *fmt, ...)
{
    if (o->len + 1 >= o->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        o->len += (size_t)n < o->cap - o->len ? (size_t)n : o->cap - o->len - 1;
    }
}

static void jkey(jout_t *o, const char *key)
{
    jput(o, "%s\"%s\":", o->items++ ? "," : "", key);
}

/* 0.1 units as a decimal number: 215 -> 21.5, -5 -> -0.5 */
static void jx10(jout_t *o, int32_t x10)
{
    jput(o, "%s%d.%d", x10 < 0 ? "-" : "", abs(x10) / 10, abs(x10) % 10);
}

static void jstr(jout_t *o, const char *s)
{
    jput(o, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            jput(o, "\\%c", c);
        } else if (c == '\n') {
            jput(o, "\\n");
        } else if (c >= 0x20) {
            jput(o, "%c", c);
        } /* other control characters (\r, colour codes) are dropped */
    }
    jput(o, "\"");
}

static void jschedule(jout_t *o, const uint8_t *sched)
{
    jput(o, "{");
    for (uint8_t d = 0; d < SCHED_DAYS; d++) {
        jput(o, "%s\"%s\":[", d ? "," : "", s_days[d]);
        for (uint8_t i = 0; i < SCHED_POINTS; i++) {
            uint8_t h, m;
            int16_t t;
            schedule_get_point(sched, d, i, &h, &m, &t);
            jput(o, "%s[\"%02u:%02u\",", i ? "," : "", h, m);
            jx10(o, t);
            jput(o, "]");
        }
        jput(o, "]");
    }
    jput(o, "}");
}

/* "06:00 55; 08:30 40; 17:00 55; 22:30 40" */
static void jday_text(jout_t *o, const uint8_t *sched, uint8_t d)
{
    char line[96];
    int  n = 0;
    for (uint8_t i = 0; i < SCHED_POINTS; i++) {
        uint8_t h, m;
        int16_t t;
        schedule_get_point(sched, d, i, &h, &m, &t);
        n += snprintf(line + n, sizeof(line) - n, "%s%02u:%02u %d", i ? "; " : "", h, m, t / 10);
        if (t % 10) {
            n += snprintf(line + n, sizeof(line) - n, ".%d", abs(t % 10));
        }
    }
    jstr(o, line);
}

/* Settings as client attributes (fields: SET_F_*). */
static void jsettings(jout_t *o, uint32_t fields)
{
    settings_t c;
    settings_get(&c);
    if (fields & SET_F_SWITCH) {
        jkey(o, "heating");
        jput(o, c.sw ? "true" : "false");
    }
    if (fields & SET_F_MODE) {
        jkey(o, "mode");
        jstr(o, s_modes[c.mode]);
    }
    if (fields & SET_F_TEMP_SET) {
        jkey(o, "temp_set");
        jx10(o, c.temp_set_x10);
    }
    if (fields & SET_F_BOILER_SET) {
        jkey(o, "boiler_set");
        jx10(o, c.boiler_set_x10);
    }
    if (fields & SET_F_LOWER) {
        jkey(o, "lower");
        jx10(o, c.lower_x10);
    }
    if (fields & SET_F_UPPER) {
        jkey(o, "upper");
        jx10(o, c.upper_x10);
    }
    if (fields & SET_F_SENSOR) {
        jkey(o, "sensor");
        jstr(o, s_sensors[c.control_sensor <= SENSOR_SEL_AVG ? c.control_sensor : 0]);
    }
    if (fields & SET_F_TZ) {
        jkey(o, "tz");
        jput(o, "%d", c.tz_min);
    }
    if (fields & SET_F_SCHEDULE) {
        jkey(o, "schedule");
        jschedule(o, c.schedule);
        for (uint8_t d = 0; d < SCHED_DAYS; d++) {
            char key[12];
            snprintf(key, sizeof(key), "sched_%s", s_days[d]);
            jkey(o, key);
            jday_text(o, c.schedule, d);
        }
    }
}

/* ------------------------------------------------------------ telemetry */

typedef enum {
    T_X10,  /* 0.1 units */
    T_INT,
    T_BOOL,
    T_WORK, /* work_state_e as a string */
    T_X100, /* 0.01 units (energy) */
    T_BERR, /* boiler fault text from (ASF flags << 8 | OEM code) */
} tele_type_e;

typedef struct {
    const char *key;
    tele_type_e type;
    int32_t     band; /* sent when it moves at least this much */
} tele_def_t;

enum {
    K_ROOM, K_TARGET, K_FLOW_SET, K_FLOW, K_MOD, K_STATE, K_DEMAND, K_FLAME, K_FAULTS, K_OT_OK, K_OEM,
    K_BFAULT, K_BERR,
    K_T1, K_T2, K_TBOARD, K_PRESSURE, K_POWER, K_ENERGY, K_RSSI, K_COUNT
};

static const tele_def_t s_tele[K_COUNT] = {
    [K_ROOM] = {"room", T_X10, 2},          [K_TARGET] = {"target", T_X10, 1},
    [K_FLOW_SET] = {"flow_set", T_X10, 5},  [K_FLOW] = {"flow", T_X10, 5},
    [K_MOD] = {"modulation", T_INT, 5},     [K_STATE] = {"state", T_WORK, 1},
    [K_DEMAND] = {"demand", T_BOOL, 1},     [K_FLAME] = {"flame", T_BOOL, 1},
    [K_FAULTS] = {"faults", T_INT, 1},      [K_OT_OK] = {"ot_ok", T_BOOL, 1},
    [K_OEM] = {"oem_fault", T_INT, 1},      [K_T1] = {"t1", T_X10, 2},
    [K_BFAULT] = {"boiler_fault", T_BOOL, 1}, [K_BERR] = {"boiler_error", T_BERR, 1},
    [K_T2] = {"t2", T_X10, 2},              [K_TBOARD] = {"t_board", T_X10, 2},
    [K_PRESSURE] = {"pressure", T_INT, 1},  [K_POWER] = {"power", T_INT, 100},
    [K_ENERGY] = {"energy", T_X100, 1},     [K_RSSI] = {"rssi", T_INT, 6},
};

typedef struct {
    bool    valid;
    int32_t v;
} tval_t;

static tval_t   s_sent[K_COUNT];
static bool     s_sent_ok[K_COUNT];
static uint32_t s_last_full, s_last_check, s_energy_ms;

static void collect(tval_t *t)
{
    regulator_status_t r;
    regulator_get_status(&r);
    boiler_status_t b;
    boiler_get_status(&b);
    memset(t, 0, sizeof(tval_t) * K_COUNT);

    t[K_ROOM]     = (tval_t){r.sensor_ok, r.room_x10};
    t[K_TARGET]   = (tval_t){true, r.target_x10};
    t[K_FLOW_SET] = (tval_t){true, r.flow_set_x10};
    t[K_FLOW]     = (tval_t){b.flow_valid, b.flow_x10};
    t[K_MOD]      = (tval_t){b.mod_valid, b.rel_mod_x10 / 10};
    t[K_STATE]    = (tval_t){true, r.work_state};
    t[K_DEMAND]   = (tval_t){true, r.demand};
    t[K_FLAME]    = (tval_t){b.comm_ok, b.flame};
    t[K_FAULTS]   = (tval_t){true, (int32_t)r.faults};
    t[K_OT_OK]    = (tval_t){true, b.comm_ok};
    t[K_OEM]      = (tval_t){b.comm_ok, b.fault ? b.oem_fault : 0};
    /* without a link the fault state is unknown: no fault, text says so */
    t[K_BFAULT] = (tval_t){true, b.comm_ok && b.fault};
    t[K_BERR]   = (tval_t){true, !b.comm_ok ? -1 : b.fault ? (int32_t)((b.asf_flags << 8) | b.oem_fault) | 0x10000 : 0};
    for (uint8_t i = 0; i < 3; i++) {
        int16_t v;
        t[K_T1 + i].valid = sensors_get(i, &v);
        t[K_T1 + i].v     = v;
    }
    int32_t pa = 0;
    t[K_PRESSURE].valid = bmp180_pressure(&pa);
    t[K_PRESSURE].v     = (pa + 50) / 100;
    t[K_POWER]          = (tval_t){true, (int32_t)energy_power_w()};
    t[K_ENERGY]         = (tval_t){true, (int32_t)energy_total_centi_kwh()};
    int8_t rssi;
    t[K_RSSI].valid = net_rssi(&rssi);
    t[K_RSSI].v     = rssi;
}

/* OpenTherm ID5: application-specific fault flags (high byte) and the
 * boiler's own (OEM) fault code (low byte). */
static void jboiler_error(jout_t *o, int32_t v)
{
    static const char *const flags[] = {"нужно обслуживание", "блокировка, нужен сброс", "низкое давление воды",
                                        "ошибка горения/нагрева", "давление воздуха", "перегрев теплоносителя"};
    char  text[200] = "";
    int   n         = 0;
    if (v <= 0) {
        jstr(o, v ? "неизвестно: нет связи с котлом" : "нет");
        return;
    }
    uint8_t asf = (uint8_t)(v >> 8), oem = (uint8_t)v;
    for (int i = 0; i < 6; i++) {
        if (asf & (1u << i)) {
            n += snprintf(text + n, sizeof(text) - n, "%s%s", n ? ", " : "", flags[i]);
        }
    }
    if (oem || !n) {
        snprintf(text + n, sizeof(text) - n, "%sкод котла %u", n ? ", " : "", oem);
    }
    jstr(o, text);
}

static void jtele(jout_t *o, int k, int32_t v)
{
    jkey(o, s_tele[k].key);
    switch (s_tele[k].type) {
    case T_X10:
        jx10(o, v);
        break;
    case T_X100:
        jput(o, "%d.%02d", v / 100, v % 100);
        break;
    case T_BOOL:
        jput(o, v ? "true" : "false");
        break;
    case T_WORK:
        jstr(o, s_work[v >= 0 && v <= WORK_IDLE ? v : WORK_IDLE]);
        break;
    case T_BERR:
        jboiler_error(o, v);
        break;
    default:
        jput(o, "%d", v);
        break;
    }
}

static void send_telemetry(bool force)
{
    static char buf[1024];
    tval_t      cur[K_COUNT];
    uint32_t    now  = tal_system_get_millisecond();
    bool        full = force || now - s_last_full >= TELE_FULL_MS;
    if (full) {
        s_last_full = now;
    }
    collect(cur);

    jout_t o = {.buf = buf, .cap = sizeof(buf)};
    jput(&o, "{");
    int     n = 0;
    uint8_t sent[K_COUNT];
    for (int k = 0; k < K_COUNT; k++) {
        if (!cur[k].valid) {
            continue;
        }
        bool due = full || !s_sent_ok[k] || abs(cur[k].v - s_sent[k].v) >= s_tele[k].band;
        if (k == K_ENERGY && !full) {
            /* the counter creeps up while heating: at most once a minute */
            due = due && now - s_energy_ms >= ENERGY_MIN_MS;
        }
        if (due) {
            jtele(&o, k, cur[k].v);
            sent[n++] = (uint8_t)k;
        }
    }
    jput(&o, "}");
    if (!n) {
        return;
    }
    if (mqtt_port_publish(TOPIC_TELE, buf, (int)o.len, 1) == OT_PORT_OK) {
        for (int i = 0; i < n; i++) {
            s_sent[sent[i]]    = cur[sent[i]];
            s_sent_ok[sent[i]] = true;
            if (sent[i] == K_ENERGY) {
                s_energy_ms = now;
            }
        }
    }
}

static void send_attributes(uint32_t fields, bool info)
{
    static char buf[2048];
    jout_t      o = {.buf = buf, .cap = sizeof(buf)};
    jput(&o, "{");
    jsettings(&o, fields);
    if (info) {
        char ip[16] = "", ssid[33] = "";
        sys_port_sta_ip(ip, sizeof(ip));
        net_get_ssid(ssid, sizeof(ssid));
        jkey(&o, "fw");
        jstr(&o, PROJECT_VERSION);
        jkey(&o, "ip");
        jstr(&o, ip);
        jkey(&o, "wifi");
        jstr(&o, ssid);
        jkey(&o, "reset");
        jstr(&o, sys_port_reset_reason());
    }
    jput(&o, "}");
    if (o.items) {
        mqtt_port_publish(TOPIC_ATTR, buf, (int)o.len, 1);
    }
}

/* ------------------------------------------------------------------ RPC */

static int16_t num_x10(const cJSON *p)
{
    double d = cJSON_IsNumber(p) ? p->valuedouble : cJSON_IsString(p) ? atof(p->valuestring) : 0;
    return (int16_t)(d * 10.0 + (d >= 0 ? 0.5 : -0.5));
}

static bool parse_bool(const cJSON *p, bool *out)
{
    if (cJSON_IsBool(p)) {
        *out = cJSON_IsTrue(p);
    } else if (cJSON_IsNumber(p)) {
        *out = p->valuedouble != 0;
    } else if (cJSON_IsString(p)) {
        *out = !strcmp(p->valuestring, "on") || !strcmp(p->valuestring, "true") || !strcmp(p->valuestring, "1");
    } else {
        return false;
    }
    return true;
}

static int index_of(const char *const *names, int n, const cJSON *p)
{
    for (int i = 0; cJSON_IsString(p) && i < n; i++) {
        if (!strcmp(p->valuestring, names[i])) {
            return i;
        }
    }
    return -1;
}

/* {"mon":[["06:00",55],...x4],...x7}; all-or-nothing. */
static bool parse_schedule(const cJSON *p, uint8_t *sched)
{
    for (uint8_t d = 0; d < SCHED_DAYS; d++) {
        const cJSON *day = cJSON_GetObjectItem(p, s_days[d]);
        if (!cJSON_IsArray(day) || cJSON_GetArraySize(day) != SCHED_POINTS) {
            return false;
        }
        for (uint8_t i = 0; i < SCHED_POINTS; i++) {
            const cJSON *pt = cJSON_GetArrayItem(day, i);
            const cJSON *tm = cJSON_GetArrayItem(pt, 0);
            int          h, m;
            if (!cJSON_IsArray(pt) || !cJSON_IsString(tm) || sscanf(tm->valuestring, "%d:%d", &h, &m) != 2 ||
                h < 0 || h > 23 || m < 0 || m > 59) {
                return false;
            }
            schedule_set_point(sched, d, i, (uint8_t)h, (uint8_t)m, num_x10(cJSON_GetArrayItem(pt, 1)));
        }
    }
    return schedule_validate(sched, SCHED_BYTES);
}

/* One day from text: 1..4 points "HH:MM temp" separated by ';' or ','
 * (decimal point, not comma); missing points repeat the last one. */
static bool parse_day_text(const char *txt, uint8_t *sched, uint8_t day)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", txt);
    char   *save = NULL;
    uint8_t n    = 0, h = 0, m = 0;
    int16_t t    = 0;
    for (char *tok = strtok_r(buf, ";,", &save); tok; tok = strtok_r(NULL, ";,", &save)) {
        for (char *c = tok; *c; c++) {
            if (*c == '=') {
                *c = ' ';
            }
        }
        int   hh, mm;
        float v;
        if (sscanf(tok, " %d:%d %f", &hh, &mm, &v) != 3 || hh < 0 || hh > 23 || mm < 0 || mm > 59 ||
            n >= SCHED_POINTS) {
            return false;
        }
        h = (uint8_t)hh;
        m = (uint8_t)mm;
        t = (int16_t)(v * 10.0f + (v >= 0 ? 0.5f : -0.5f));
        schedule_set_point(sched, day, n++, h, m, t);
    }
    if (!n) {
        return false;
    }
    for (; n < SCHED_POINTS; n++) {
        schedule_set_point(sched, day, n, h, m, t);
    }
    return true;
}

/* Runs a console line and returns what it printed. */
static void rpc_cli(jout_t *o, const cJSON *p)
{
    if (!cJSON_IsString(p)) {
        jput(o, "{\"error\":\"params: command line string\"}");
        return;
    }
    uint32_t mark = log_ring_mark();
    int      rc   = app_cli_exec(p->valuestring);
    char    *text = tal_malloc(CLI_OUT_MAX + 1);
    if (!text) {
        jput(o, "{\"error\":\"no memory\"}");
        return;
    }
    size_t n = log_ring_since(mark, text, CLI_OUT_MAX);
    text[n]  = 0;
    jput(o, "{\"ok\":%s,\"out\":", rc == 0 ? "true" : "false");
    jstr(o, text);
    jput(o, "}");
    tal_free(text);
}

static void rpc_dispatch(const char *method, const cJSON *p, jout_t *o)
{
    const settings_src_e SRC = SETTINGS_SRC_CLOUD;
    settings_t           c;
    bool                 b;
    int                  i;
    uint32_t             fields = 0; /* settings to return */

    if (!strcmp(method, "getState")) {
        fields = SET_F_ALL;
    } else if (!strcmp(method, "setHeating") && parse_bool(p, &b)) {
        settings_set_switch(b, SRC);
        fields = SET_F_SWITCH;
    } else if (!strcmp(method, "getHeating")) {
        fields = SET_F_SWITCH;
    } else if (!strcmp(method, "setMode") && (i = index_of(s_modes, HEAT_MODE_COUNT, p)) >= 0) {
        settings_set_mode((uint8_t)i, SRC);
        fields = SET_F_MODE;
    } else if (!strcmp(method, "getMode")) {
        fields = SET_F_MODE;
    } else if (!strcmp(method, "setTemp") && (cJSON_IsNumber(p) || cJSON_IsString(p))) {
        settings_set_temp_set(num_x10(p), SRC);
        fields = SET_F_TEMP_SET;
    } else if (!strcmp(method, "getTemp")) {
        fields = SET_F_TEMP_SET;
    } else if (!strcmp(method, "setBoiler") && (cJSON_IsNumber(p) || cJSON_IsString(p))) {
        if (!settings_set_boiler_set(num_x10(p), SRC)) {
            jput(o, "{\"error\":\"boiler setpoint 20..80\"}");
            return;
        }
        fields = SET_F_BOILER_SET;
    } else if (!strcmp(method, "getBoiler")) {
        fields = SET_F_BOILER_SET;
    } else if (!strcmp(method, "setLimits")) {
        const cJSON *lo = cJSON_GetObjectItem(p, "lower"), *hi = cJSON_GetObjectItem(p, "upper");
        if ((lo && !settings_set_lower(num_x10(lo), SRC)) || (hi && !settings_set_upper(num_x10(hi), SRC))) {
            jput(o, "{\"error\":\"lower 5..15, upper 25..35\"}");
            return;
        }
        fields = SET_F_LOWER | SET_F_UPPER | SET_F_TEMP_SET;
    } else if (!strcmp(method, "setSensor") && (i = index_of(s_sensors, 6, p)) >= 0) {
        settings_set_control_sensor((uint8_t)i, SRC);
        fields = SET_F_SENSOR;
    } else if (!strcmp(method, "setTz") && cJSON_IsNumber(p)) {
        if (!settings_set_tz((int16_t)p->valueint, SRC)) {
            jput(o, "{\"error\":\"tz: minutes from UTC, -720..840\"}");
            return;
        }
        fields = SET_F_TZ;
    } else if (!strcmp(method, "setSchedule")) {
        settings_get(&c);
        if (!cJSON_IsObject(p) || !parse_schedule(p, c.schedule) ||
            !settings_set_schedule(c.schedule, SCHED_BYTES, SRC)) {
            jput(o, "{\"error\":\"schedule: {\\\"mon\\\":[[\\\"06:00\\\",55],x4],...,\\\"sun\\\":[...]}\"}");
            return;
        }
        fields = SET_F_SCHEDULE;
    } else if (!strcmp(method, "getSchedule")) {
        fields = SET_F_SCHEDULE;
    } else if (!strcmp(method, "cli")) {
        rpc_cli(o, p);
        return;
    } else {
        jput(o, "{\"error\":\"unknown method or bad params\"}");
        return;
    }

    /* A single field answers with the bare value (what dashboard control
     * widgets expect), several with an object. */
    jout_t v = {.buf = o->buf, .cap = o->cap, .len = o->len};
    jput(&v, "{");
    jsettings(&v, fields);
    jput(&v, "}");
    if (v.items == 1) {
        char *colon = strchr(o->buf + o->len, ':');
        size_t vlen = v.len - 1 - (size_t)(colon + 1 - o->buf); /* without the closing brace */
        memmove(o->buf + o->len, colon + 1, vlen);
        o->len += vlen;
        o->buf[o->len] = 0;
    } else {
        o->len = v.len;
    }
}

/* ---------------------------------------------------- shared attributes */

/* set_sched_all before the single days: a day set together with it wins */
static const char *const s_shared_keys[] = {
    "set_heating",   "set_mode",      "set_temp",      "set_boiler",    "set_lower",     "set_upper",
    "set_sensor",    "set_tz",        "set_schedule",  "set_sched_all", "set_sched_mon", "set_sched_tue",
    "set_sched_wed", "set_sched_thu", "set_sched_fri", "set_sched_sat", "set_sched_sun"};

static cJSON *s_seen; /* {"set_temp":21.5,...}, client task only */

static void seen_load(void)
{
    uint8_t *buf = NULL;
    size_t   len = 0;
    if (tal_kv_get(KV_SEEN, &buf, &len) == OPRT_OK) {
        s_seen = cJSON_ParseWithLength((const char *)buf, len);
        tal_kv_free(buf);
    }
    if (!cJSON_IsObject(s_seen)) {
        cJSON_Delete(s_seen);
        s_seen = cJSON_CreateObject();
    }
}

static void seen_save(void)
{
    char *json = cJSON_PrintUnformatted(s_seen);
    if (!json || tal_kv_set(KV_SEEN, (const uint8_t *)json, strlen(json)) != OPRT_OK) {
        PR_ERR("cloud: saving shared attribute state failed");
    }
    cJSON_free(json);
}

/* Applies one set_* value; false if the key is unknown or the value bad. */
static bool shared_apply(const char *key, const cJSON *v)
{
    const settings_src_e SRC = SETTINGS_SRC_CLOUD;
    bool                 b;
    int                  i;

    if (!strcmp(key, "set_heating") && parse_bool(v, &b)) {
        settings_set_switch(b, SRC);
        return true;
    }
    if (!strcmp(key, "set_mode") && (i = index_of(s_modes, HEAT_MODE_COUNT, v)) >= 0) {
        return settings_set_mode((uint8_t)i, SRC);
    }
    if (!strcmp(key, "set_temp") && (cJSON_IsNumber(v) || cJSON_IsString(v))) {
        settings_set_temp_set(num_x10(v), SRC);
        return true;
    }
    if (!strcmp(key, "set_boiler") && (cJSON_IsNumber(v) || cJSON_IsString(v))) {
        return settings_set_boiler_set(num_x10(v), SRC);
    }
    if (!strcmp(key, "set_lower") && (cJSON_IsNumber(v) || cJSON_IsString(v))) {
        return settings_set_lower(num_x10(v), SRC);
    }
    if (!strcmp(key, "set_upper") && (cJSON_IsNumber(v) || cJSON_IsString(v))) {
        return settings_set_upper(num_x10(v), SRC);
    }
    if (!strcmp(key, "set_sensor")) {
        /* "1".."4", "min", "avg"; ThingsBoard may turn "1" into a number */
        i = cJSON_IsNumber(v) ? v->valueint - 1 : index_of(s_sensors, 6, v);
        return i >= 0 && i <= SENSOR_SEL_AVG && settings_set_control_sensor((uint8_t)i, SRC);
    }
    if (!strcmp(key, "set_tz") && cJSON_IsNumber(v)) {
        return settings_set_tz((int16_t)v->valueint, SRC);
    }
    if (!strncmp(key, "set_sched_", 10) && cJSON_IsString(v)) {
        settings_t c;
        settings_get(&c);
        bool all = !strcmp(key + 10, "all");
        bool ok  = false;
        for (uint8_t d = 0; d < SCHED_DAYS; d++) {
            if (all || !strcmp(key + 10, s_days[d])) {
                ok = parse_day_text(v->valuestring, c.schedule, d);
                if (!ok) {
                    break;
                }
            }
        }
        return ok && schedule_validate(c.schedule, SCHED_BYTES) &&
               settings_set_schedule(c.schedule, SCHED_BYTES, SRC);
    }
    if (!strcmp(key, "set_schedule")) {
        /* a JSON attribute, or a string holding the JSON */
        cJSON     *parsed = cJSON_IsString(v) ? cJSON_Parse(v->valuestring) : NULL;
        const cJSON *obj  = parsed ? parsed : v;
        settings_t c;
        settings_get(&c);
        bool ok = cJSON_IsObject(obj) && parse_schedule(obj, c.schedule) &&
                  settings_set_schedule(c.schedule, SCHED_BYTES, SRC);
        cJSON_Delete(parsed);
        return ok;
    }
    return false;
}

/* fresh: a push about a change just made on the server (always applied);
 * otherwise the full set requested on connect (only changed values). */
static void shared_handle(const cJSON *obj, bool fresh)
{
    bool dirty = false;
    fw_update_on_shared(obj);
    for (size_t k = 0; k < sizeof(s_shared_keys) / sizeof(s_shared_keys[0]); k++) {
        const char  *key = s_shared_keys[k];
        const cJSON *v   = cJSON_GetObjectItem(obj, key);
        if (!v) {
            continue;
        }
        const cJSON *old = cJSON_GetObjectItem(s_seen, key);
        if (!fresh && old && cJSON_Compare(old, v, true)) {
            continue; /* already applied; a local change since then wins */
        }
        bool ok = shared_apply(key, v);
        PR_NOTICE("cloud: shared %s %s", key, ok ? "applied" : "REJECTED");
        if (!ok) {
            /* the dashboard shows the real value from the client attributes */
            cloud_report_settings(SET_F_ALL);
        }
        cJSON_DeleteItemFromObject(s_seen, key);
        cJSON_AddItemToObject(s_seen, key, cJSON_Duplicate(v, true));
        dirty = true;
    }
    if (dirty) {
        seen_save();
    }
}

static void on_message(const char *topic, int topic_len, const char *data, int data_len)
{
    if (fw_update_on_message(topic, topic_len, data, data_len)) {
        return;
    }
    if (topic_len == (int)strlen(TOPIC_ATTR) && !strncmp(topic, TOPIC_ATTR, (size_t)topic_len)) {
        cJSON *obj = cJSON_ParseWithLength(data, (size_t)data_len);
        shared_handle(obj, true);
        cJSON_Delete(obj);
        return;
    }
    if (topic_len > (int)strlen(TOPIC_ARSP) && !strncmp(topic, TOPIC_ARSP, strlen(TOPIC_ARSP))) {
        cJSON *obj = cJSON_ParseWithLength(data, (size_t)data_len);
        shared_handle(cJSON_GetObjectItem(obj, "shared"), false);
        cJSON_Delete(obj);
        return;
    }

    const size_t plen = strlen(TOPIC_RPC);
    if (topic_len <= (int)plen || topic_len > (int)plen + 10 || strncmp(topic, TOPIC_RPC, plen) != 0) {
        return;
    }
    char resp_topic[48];
    snprintf(resp_topic, sizeof(resp_topic), "%s%.*s", TOPIC_RESP, topic_len - (int)plen, topic + plen);

    cJSON       *req    = cJSON_ParseWithLength(data, (size_t)data_len);
    const cJSON *method = cJSON_GetObjectItem(req, "method");
    char        *buf    = tal_malloc(2048);
    if (buf) {
        jout_t o = {.buf = buf, .cap = 2048};
        if (cJSON_IsString(method)) {
            PR_NOTICE("cloud: rpc %s", method->valuestring);
            rpc_dispatch(method->valuestring, cJSON_GetObjectItem(req, "params"), &o);
        } else {
            jput(&o, "{\"error\":\"bad request\"}");
        }
        mqtt_port_publish(resp_topic, buf, (int)o.len, 1);
        tal_free(buf);
    }
    cJSON_Delete(req);
}

/* ----------------------------------------------------------- connection */

static volatile bool     s_connected_ev; /* set by the client task */
static volatile uint32_t s_pending_settings;
static volatile uint32_t s_pending_status;
static bool              s_started;

static void on_conn(bool connected)
{
    if (connected) {
        s_connected_ev = true;
    } else {
        PR_WARN("cloud: offline, heating keeps running locally");
    }
}

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
    cloud_report_settings(SET_F_ALL);
    s_last_full = tal_system_get_millisecond() - TELE_FULL_MS; /* next check sends everything */
}

bool cloud_is_online(void)
{
    return mqtt_port_connected();
}

cloud_state_e cloud_state(void)
{
    if (net_setup_active()) {
        return CLOUD_SETUP;
    }
    return cloud_is_online() ? CLOUD_ONLINE : CLOUD_OFFLINE;
}

static void client_start(void)
{
    static char uri[96], token[64], pass[64], client_id[24];

    tal_mutex_lock(s_cfg_lock);
    snprintf(uri, sizeof(uri), "%s://%s:%u", s_cfg.port == 1883 ? "mqtt" : "mqtts", s_cfg.host, s_cfg.port);
    snprintf(token, sizeof(token), "%s", s_cfg.token);
    snprintf(pass, sizeof(pass), "%s", s_cfg.pass);
    tal_mutex_unlock(s_cfg_lock);

    NW_MAC_S mac = {0};
    tal_wifi_get_mac(WF_STATION, &mac);
    snprintf(client_id, sizeof(client_id), "opentherm-%02x%02x%02x", mac.mac[3], mac.mac[4], mac.mac[5]);

    mqtt_port_cfg_t cfg = {
        .uri          = uri,
        .username     = token,
        .password     = pass[0] ? pass : NULL,
        .client_id    = client_id,
        .keepalive_s  = KEEPALIVE_S,
        .reconnect_ms = RECONNECT_MS,
        .on_conn      = on_conn,
        .on_msg       = on_message,
    };
    if (mqtt_port_start(&cfg) == OT_PORT_OK) {
        s_started = true;
        PR_NOTICE("cloud: connecting to %s", uri);
    } else {
        PR_ERR("cloud: client start failed");
    }
}

void cloud_init(void)
{
    tal_mutex_create_init(&s_cfg_lock);
    cfg_load();
    seen_load();
    fw_update_init();
    if (!s_cfg.host[0] || !s_cfg.token[0]) {
        PR_WARN("cloud: no ThingsBoard server/token; set with: tb host <name> [port] / tb token <token>");
    }
}

void cloud_run(void)
{
    for (;;) {
        if (s_restart) {
            s_restart = false;
            mqtt_port_stop();
            s_started = false;
        }
        if (!s_started && net_is_up() && s_cfg.host[0] && s_cfg.token[0]) {
            client_start();
        }

        if (s_connected_ev) {
            s_connected_ev = false;
            PR_NOTICE("cloud: online");
            mqtt_port_subscribe(TOPIC_RPC "+", 1);
            mqtt_port_subscribe(TOPIC_ATTR, 1);
            mqtt_port_subscribe(TOPIC_ARSP "+", 1);
            if (fw_update_on_connect()) {
                /* downloading firmware: only resume it, changes still go out below */
                continue;
            }
            send_attributes(SET_F_ALL, true);
            {
                /* commands set while the device was offline, firmware package */
                static const char req[] =
                    "{\"sharedKeys\":\"set_heating,set_mode,set_temp,set_boiler,set_lower,set_upper,set_sensor,"
                    "set_tz,set_schedule,set_sched_all,set_sched_mon,set_sched_tue,set_sched_wed,set_sched_thu,"
                    "set_sched_fri,set_sched_sat,set_sched_sun," FW_UPDATE_SHARED_KEYS "\"}";
                mqtt_port_publish(TOPIC_AREQ, req, (int)strlen(req), 1);
            }
            __atomic_fetch_and(&s_pending_settings, 0, __ATOMIC_SEQ_CST);
            send_telemetry(true);
        }

        if (mqtt_port_connected()) {
            uint32_t s = __atomic_exchange_n(&s_pending_settings, 0, __ATOMIC_SEQ_CST);
            uint32_t r = __atomic_exchange_n(&s_pending_status, 0, __ATOMIC_SEQ_CST);
            if (s) {
                send_attributes(s, false);
            }
            uint32_t now = tal_system_get_millisecond();
            if (r || now - s_last_check >= TELE_CHECK_MS) {
                s_last_check = now;
                send_telemetry(false);
            }
        }
        fw_update_tick();
        tal_system_sleep(TICK_MS);
    }
}
