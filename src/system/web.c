/**
 * @file web.c
 * @brief Minimal local web UI: status page with forms (GET /, POST /set).
 *
 * Plain server-rendered HTML, no JavaScript. Every change goes through the
 * same settings setters as the CLI (source LOCAL), so it is persisted and
 * reported to ThingsBoard like any other local change. In setup mode the same
 * page, served on the access point, is where Wi-Fi and the token are entered.
 */

#include "web.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tal_api.h"
#include "tuya_kconfig.h"

#include "bmp180.h"
#include "boiler.h"
#include "clock.h"
#include "cloud.h"
#include "energy.h"
#include "net.h"
#include "ot_port.h"
#include "regulator.h"
#include "sensors.h"
#include "settings.h"
#include "supervisor.h"

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "?"
#endif

static const char *const s_modes[HEAT_MODE_COUNT] = {"auto", "manual", "home", "leave"};
static const char *const s_mode_ru[HEAT_MODE_COUNT] = {"авто (расписание = уставки подачи)",
                                                       "ручной (уставка котла)", "дом (комната = temp_set)",
                                                       "отъезд (комната 16 °C)"};
static const char *const s_sensor_ru[] = {"датчик 1 (J2)", "датчик 2 (J3)", "на плате (BMP180)", "датчик 4",
                                          "минимум", "среднее"};
static const char *const s_work_ru[] = {"нагрев", "ожидание нагрева", "простой"};
static const char *const s_day_ru[SCHED_DAYS] = {"Пн", "Вт", "Ср", "Чт", "Пт", "Сб", "Вс"};

/* ------------------------------------------------------------ rendering */

typedef struct {
    char  *buf;
    size_t cap, len;
} out_t;

static void put(out_t *o, const char *fmt, ...)
{
    if (o->len >= o->cap) {
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

static void put_temp(out_t *o, bool ok, int16_t x10)
{
    if (ok) {
        put(o, "%s%d.%d °C", x10 < 0 ? "-" : "", abs(x10) / 10, abs(x10) % 10);
    } else {
        put(o, "—");
    }
}

static void row(out_t *o, const char *name)
{
    put(o, "<tr><td>%s</td><td>", name);
}

size_t web_page(char *buf, size_t cap)
{
    out_t o = {.buf = buf, .cap = cap, .len = 0};

    settings_t c;
    settings_get(&c);
    regulator_status_t r;
    regulator_get_status(&r);
    boiler_status_t b;
    boiler_get_status(&b);
    uint8_t  wd = 0;
    uint16_t mn = 0;
    bool     tv = clock_now(&wd, &mn);
    char     ip[16] = "—";
    sys_port_sta_ip(ip, sizeof(ip));

    put(&o, "<!DOCTYPE html><html><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>OpenTherm</title><style>"
            "body{font-family:sans-serif;max-width:720px;margin:auto;padding:8px}"
            "table{border-collapse:collapse;width:100%%;margin-bottom:12px}"
            "td{border-bottom:1px solid #ddd;padding:4px}td:first-child{width:45%%;color:#555}"
            "input[type=text],input[type=number],select{width:7em}"
            ".f{color:#b00;font-weight:bold}</style></head><body>");
    put(&o, "<h2>OpenTherm-термостат</h2><p><a href='/'>обновить</a> · <a href='/log'>лог</a></p>");

    /* status */
    put(&o, "<h3>Состояние</h3><table>");
    row(&o, "Отопление");
    put(&o, "%s</td></tr>", c.sw ? "включено" : "выключено (только защита от замерзания)");
    row(&o, "Режим");
    put(&o, "%s</td></tr>", s_mode_ru[c.mode]);
    row(&o, "Цель");
    put(&o, "%d.%d °C (%s)</td></tr>", r.target_x10 / 10, abs(r.target_x10) % 10, r.target_src ? r.target_src : "—");
    row(&o, "Уставка подачи → котёл");
    put(&o, "%s %d.%d °C</td></tr>", r.demand ? "нагрев" : "без нагрева,", r.flow_set_x10 / 10, r.flow_set_x10 % 10);
    row(&o, "Состояние");
    put(&o, "%s</td></tr>", s_work_ru[r.work_state]);
    row(&o, "Ошибки");
    if (r.faults) {
        put(&o, "<span class=f>%s%s</span>", (r.faults & FAULT_SENSOR) ? "нет датчика; " : "",
            (r.faults & FAULT_HEATING) ? (b.comm_ok ? "ошибка котла" : "нет связи с котлом") : "");
    } else {
        put(&o, "нет");
    }
    put(&o, "</td></tr>");
    row(&o, "Управляющая температура");
    put_temp(&o, r.sensor_ok, r.room_x10);
    put(&o, "</td></tr></table>");

    /* boiler */
    put(&o, "<h3>Котёл (OpenTherm)</h3><table>");
    row(&o, "Связь");
    put(&o, "%s (ошибок обмена: %u)</td></tr>", b.comm_ok ? "есть" : "<span class=f>нет</span>", b.comm_errors);
    row(&o, "Подача");
    put_temp(&o, b.flow_valid, b.flow_x10);
    put(&o, "</td></tr>");
    row(&o, "ТЭНы / модуляция");
    put(&o, "%s", b.flame ? "включены" : "выключены");
    if (b.mod_valid) {
        put(&o, ", %d%%", b.rel_mod_x10 / 10);
    }
    put(&o, "</td></tr>");
    row(&o, "Ошибка котла");
    put(&o, "%s (код OEM %u)</td></tr>", b.fault ? "<span class=f>да</span>" : "нет", b.oem_fault);
    uint32_t e = energy_total_centi_kwh();
    row(&o, "Мощность / энергия (оценка)");
    put(&o, "%u Вт / %u.%02u кВт·ч</td></tr></table>", energy_power_w(), e / 100, e % 100);

    /* sensors */
    put(&o, "<h3>Датчики</h3><table>");
    for (uint8_t i = 0; i < 3; i++) {
        int16_t v;
        bool    ok = sensors_get(i, &v);
        row(&o, s_sensor_ru[i]);
        put_temp(&o, ok, v);
        put(&o, "</td></tr>");
    }
    int32_t pa;
    row(&o, "давление");
    if (bmp180_pressure(&pa)) {
        put(&o, "%d гПа</td></tr>", (int)((pa + 50) / 100));
    } else {
        put(&o, "—</td></tr>");
    }
    put(&o, "</table>");

    /* settings form */
    put(&o, "<h3>Настройки</h3><form method=post action=/set><table>");
    row(&o, "Отопление");
    put(&o, "<select name=sw><option value=1%s>включено</option><option value=0%s>выключено</option></select></td></tr>",
        c.sw ? " selected" : "", c.sw ? "" : " selected");
    row(&o, "Режим");
    put(&o, "<select name=mode>");
    for (uint8_t m = 0; m < HEAT_MODE_COUNT; m++) {
        put(&o, "<option value=%s%s>%s</option>", s_modes[m], m == c.mode ? " selected" : "", s_mode_ru[m]);
    }
    put(&o, "</select></td></tr>");
    row(&o, "Уставка котла (ручной), °C");
    put(&o, "<input type=number name=boiler min=20 max=80 value=%d></td></tr>", c.boiler_set_x10 / 10);
    row(&o, "Комната в режиме «дом», °C");
    put(&o, "<input type=text name=temp_set value=%d.%d></td></tr>", c.temp_set_x10 / 10, c.temp_set_x10 % 10);
    row(&o, "Пределы уставки комнаты, °C");
    put(&o, "<input type=text name=lower value=%d.%d> … <input type=text name=upper value=%d.%d></td></tr>",
        c.lower_x10 / 10, c.lower_x10 % 10, c.upper_x10 / 10, c.upper_x10 % 10);
    row(&o, "Управляющий датчик");
    put(&o, "<select name=sensor>");
    for (uint8_t s = 0; s <= SENSOR_SEL_AVG; s++) {
        put(&o, "<option value=%u%s>%s</option>", s, s == c.control_sensor ? " selected" : "", s_sensor_ru[s]);
    }
    put(&o, "</select></td></tr></table><input type=hidden name=form value=settings>"
            "<button type=submit>Сохранить настройки</button></form>");

    /* schedule form */
    put(&o, "<h3>Расписание (уставки подачи; ниже 20 °C — котёл выключен)</h3>"
            "<form method=post action=/set><table>");
    for (uint8_t d = 0; d < SCHED_DAYS; d++) {
        put(&o, "<tr><td>%s%s</td><td>", s_day_ru[d], tv && d == wd ? " (сегодня)" : "");
        for (uint8_t i = 0; i < SCHED_POINTS; i++) {
            uint8_t h, m;
            int16_t t;
            schedule_get_point(c.schedule, d, i, &h, &m, &t);
            put(&o,
                "<input type=text name=t%u%u value=%02u:%02u size=5 style='width:4em'> "
                "<input type=text name=v%u%u value=%d.%d size=4 style='width:3.5em'> ",
                d, i, h, m, d, i, t / 10, t % 10);
        }
        put(&o, "</td></tr>");
    }
    put(&o, "</table><input type=hidden name=form value=schedule><button type=submit>Сохранить расписание</button>"
            "</form>");

    /* system */
    put(&o, "<h3>Система</h3><table>");
    row(&o, "Прошивка");
    put(&o, "%s (%s)</td></tr>", PROJECT_VERSION, sys_port_running_partition());
    row(&o, "Время");
    if (tv) {
        put(&o, "%s %02u:%02u</td></tr>", s_day_ru[wd], mn / 60, mn % 60);
    } else {
        put(&o, "неизвестно</td></tr>");
    }
    row(&o, "ThingsBoard");
    put(&o, "%s</td></tr>", cloud_is_online() ? "на связи" : "нет связи");
    row(&o, "Wi-Fi / IP / сигнал");
    {
        char   ssid[33];
        int8_t rssi = 0;
        net_get_ssid(ssid, sizeof(ssid));
        put(&o, "%s / %s / ", ssid[0] ? ssid : "не задан", ip);
        if (net_rssi(&rssi)) {
            put(&o, "%d дБм</td></tr>", rssi);
        } else {
            put(&o, "—</td></tr>");
        }
    }
    row(&o, "Свободная память");
    put(&o, "%d байт</td></tr>", tal_system_get_free_heap_size());
    row(&o, "Время работы / причина старта");
    put(&o, "%u с / %s</td></tr>", (unsigned)(tal_system_get_millisecond() / 1000), sys_port_reset_reason());
    row(&o, "Последнее зависание");
    put(&o, "%s</td></tr></table>", supervisor_last_stall());

    /* connection form: empty fields keep the stored values */
    char     host[64];
    uint16_t port;
    bool     tok;
    cloud_get_server(host, sizeof(host), &port, &tok);
    put(&o, "<h3>Подключение</h3><form method=post action=/set><table>");
    row(&o, "Wi-Fi: сеть");
    put(&o, "<input type=text name=ssid style='width:14em' placeholder='не менять'></td></tr>");
    row(&o, "Wi-Fi: пароль");
    put(&o, "<input type=password name=pass style='width:14em'></td></tr>");
    row(&o, "ThingsBoard: сервер");
    put(&o, "<input type=text name=host style='width:14em' value='%s'> порт <input type=number name=port value=%u>"
            "</td></tr>", host, port);
    row(&o, "ThingsBoard: токен устройства");
    put(&o, "<input type=password name=token style='width:14em' placeholder='%s'></td></tr>",
        tok ? "задан, не менять" : "не задан");
    row(&o, "Часовой пояс, минут от UTC");
    put(&o, "<input type=number name=tz value=%d></td></tr>", c.tz_min);
    put(&o, "</table><input type=hidden name=form value=net><button type=submit>Сохранить подключение</button>"
            "</form></body></html>");

    return o.len;
}

/* ---------------------------------------------------------------- forms */

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Finds name=value in an url-encoded body and decodes the value. */
static bool field(const char *body, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, name, nl) != 0 || p[nl] != '=') {
            continue;
        }
        const char *v = p + nl + 1;
        size_t      n = 0;
        while (*v && *v != '&' && n + 1 < cap) {
            if (*v == '+') {
                out[n++] = ' ';
                v++;
            } else if (*v == '%' && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                out[n++] = (char)(hexval(v[1]) * 16 + hexval(v[2]));
                v += 3;
            } else {
                out[n++] = *v++;
            }
        }
        out[n] = 0;
        return true;
    }
    return false;
}

static bool field_x10(const char *body, const char *name, int16_t *x10)
{
    char v[16];
    if (!field(body, name, v, sizeof(v)) || !v[0]) {
        return false;
    }
    for (char *c = v; *c; c++) {
        if (*c == ',') {
            *c = '.'; /* Russian decimal comma */
        }
    }
    double d = atof(v);
    *x10     = (int16_t)(d * 10.0 + (d >= 0 ? 0.5 : -0.5));
    return true;
}

static void net_form(const char *body)
{
    char     ssid[33], pass[65], host[64], token[64], v[16];
    char     cur_host[64];
    uint16_t cur_port;
    bool     tok;
    cloud_get_server(cur_host, sizeof(cur_host), &cur_port, &tok);

    if (field(body, "tz", v, sizeof(v)) && v[0]) {
        settings_set_tz((int16_t)atoi(v), SETTINGS_SRC_LOCAL);
    }
    if (field(body, "host", host, sizeof(host)) && host[0] && field(body, "port", v, sizeof(v)) && atoi(v) > 0 &&
        (strcmp(host, cur_host) != 0 || atoi(v) != cur_port)) {
        cloud_set_server(host, (uint16_t)atoi(v));
    }
    if (field(body, "token", token, sizeof(token)) && token[0]) {
        cloud_set_token(token);
    }
    /* last: it may drop the link this page came over */
    if (field(body, "ssid", ssid, sizeof(ssid)) && ssid[0] && field(body, "pass", pass, sizeof(pass))) {
        net_set_wifi(ssid, pass);
    }
}

void web_form(const char *body)
{
    char    v[16];
    int16_t x;

    if (!field(body, "form", v, sizeof(v))) {
        return;
    }
    PR_NOTICE("web: %s form submitted", v);

    if (!strcmp(v, "settings")) {
        settings_t c;
        settings_get(&c);
        if (field(body, "sw", v, sizeof(v)) && (v[0] == '1') != c.sw) {
            settings_set_switch(v[0] == '1', SETTINGS_SRC_LOCAL);
        }
        if (field(body, "mode", v, sizeof(v))) {
            for (uint8_t m = 0; m < HEAT_MODE_COUNT; m++) {
                if (!strcmp(v, s_modes[m]) && m != c.mode) {
                    settings_set_mode(m, SETTINGS_SRC_LOCAL);
                }
            }
        }
        if (field_x10(body, "boiler", &x) && x != c.boiler_set_x10) {
            settings_set_boiler_set(x, SETTINGS_SRC_LOCAL);
        }
        /* limits before temp_set so the new range applies to it */
        if (field_x10(body, "lower", &x) && x != c.lower_x10) {
            settings_set_lower(x, SETTINGS_SRC_LOCAL);
        }
        if (field_x10(body, "upper", &x) && x != c.upper_x10) {
            settings_set_upper(x, SETTINGS_SRC_LOCAL);
        }
        if (field_x10(body, "temp_set", &x) && x != c.temp_set_x10) {
            settings_set_temp_set(x, SETTINGS_SRC_LOCAL);
        }
        if (field(body, "sensor", v, sizeof(v)) && atoi(v) != c.control_sensor) {
            settings_set_control_sensor((uint8_t)atoi(v), SETTINGS_SRC_LOCAL);
        }
    } else if (!strcmp(v, "net")) {
        net_form(body);
    } else if (!strcmp(v, "schedule")) {
        settings_t c;
        settings_get(&c);
        for (uint8_t d = 0; d < SCHED_DAYS; d++) {
            for (uint8_t i = 0; i < SCHED_POINTS; i++) {
                char name[8];
                int  h, m;
                snprintf(name, sizeof(name), "t%u%u", d, i);
                if (!field(body, name, v, sizeof(v)) || sscanf(v, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 ||
                    m < 0 || m > 59) {
                    PR_WARN("web: bad time for %s", name);
                    return; /* reject the whole schedule rather than store half of it */
                }
                snprintf(name, sizeof(name), "v%u%u", d, i);
                if (!field_x10(body, name, &x)) {
                    PR_WARN("web: bad value for %s", name);
                    return;
                }
                schedule_set_point(c.schedule, d, i, (uint8_t)h, (uint8_t)m, x);
            }
        }
        if (!settings_set_schedule(c.schedule, SCHED_BYTES, SETTINGS_SRC_LOCAL)) {
            PR_WARN("web: schedule rejected");
        }
    }
}
