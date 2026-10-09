/**
 * @file cli_cmd.c
 * @brief Debug CLI over the USB serial console, also run
 *        remotely via POST /cli on the LAN and the "cli" RPC from ThingsBoard.
 *
 * Covers everything the app does, so the firmware can be exercised without
 * the cloud.
 */

#include <stdlib.h>
#include <string.h>

#include "tal_api.h"
#include "tal_cli.h"

#include "boiler.h"
#include "bmp180.h"
#include "boiler_emu.h"
#include "clock.h"
#include "energy.h"
#include "cloud.h"
#include "fw_update.h"
#include "ds18b20.h"
#include "net.h"
#include "tuya_kconfig.h"
#include "ot_frame.h"
#include "ot_port.h"
#include "regulator.h"
#include "sensors.h"
#include "settings.h"
#include "status_led.h"
#include "supervisor.h"

extern void tal_kv_cmd(int argc, char *argv[]);

static const char *const s_modes[HEAT_MODE_COUNT] = {"auto", "manual", "home", "leave"};
static const char *const s_work[]                 = {"heating", "warm", "idle"};

static int16_t parse_x10(const char *s)
{
    return (int16_t)(atof(s) * 10.0 + (atof(s) >= 0 ? 0.5 : -0.5));
}

static void cmd_status(int argc, char *argv[])
{
    settings_t c;
    settings_get(&c);
    regulator_status_t r;
    regulator_get_status(&r);
    boiler_status_t b;
    boiler_get_status(&b);
    uint8_t  wd = 0;
    uint16_t mn = 0;
    bool     tv = clock_now(&wd, &mn);

    PR_NOTICE("settings: sw=%d mode=%s set=%d boiler_set=%d range=%d..%d sensor=%d", c.sw, s_modes[c.mode],
              c.temp_set_x10, c.boiler_set_x10, c.lower_x10, c.upper_x10, c.control_sensor);
    PR_NOTICE("time:     %s day=%d %02d:%02d", tv ? "valid" : "UNKNOWN", wd + 1, mn / 60, mn % 60);
    PR_NOTICE("control:  room=%d%s target=%d (%s) demand=%d flow_set=%d I=%d faults=0x%x state=%s", r.room_x10,
              r.sensor_ok ? "" : "(NO SENSOR)", r.target_x10, r.target_src ? r.target_src : "-", r.demand,
              r.flow_set_x10, r.integral_x10, r.faults, s_work[r.work_state]);
    PR_NOTICE("boiler:   comm=%d err=%u ch_cmd=%d tset=%d | fault=%d ch=%d flame=%d flow=%d%s mod=%d%s asf=0x%02x "
              "oem=%u",
              b.comm_ok, b.comm_errors, b.ch_enable_cmd, b.tset_x10_cmd, b.fault, b.ch_active, b.flame, b.flow_x10,
              b.flow_valid ? "" : "?", b.rel_mod_x10, b.mod_valid ? "" : "?", b.asf_flags, b.oem_fault);
    char   ip[16] = "-", ssid[33];
    int8_t rssi   = 0;
    sys_port_sta_ip(ip, sizeof(ip));
    net_get_ssid(ssid, sizeof(ssid));
    net_rssi(&rssi);
    PR_NOTICE("network:  wifi=%s up=%d setup_ap=%d ip=%s rssi=%d cloud=%d heap=%d", ssid, net_is_up(),
              net_setup_active(), ip, rssi, cloud_is_online(), tal_system_get_free_heap_size());
    /* SYS_TIME_T is 64-bit here and PR_* has no format checking: cast. */
    PR_NOTICE("system:   uptime=%us reset=%s app=%s last_stall=%s", (unsigned)(tal_system_get_millisecond() / 1000),
              sys_port_reset_reason(), sys_port_running_partition(), supervisor_last_stall());
}

static void cmd_heat(int argc, char *argv[])
{
    if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        settings_set_switch(!strcmp(argv[1], "on"), SETTINGS_SRC_LOCAL);
    } else {
        PR_INFO("usage: heat <on|off>");
    }
}

static void cmd_set(int argc, char *argv[])
{
    if (argc != 2) {
        PR_INFO("usage: set <room setpoint degC, e.g. 21.5>");
        return;
    }
    settings_set_temp_set(parse_x10(argv[1]), SETTINGS_SRC_LOCAL);
}

static void cmd_boiler(int argc, char *argv[])
{
    if (argc != 2 || !settings_set_boiler_set(parse_x10(argv[1]), SETTINGS_SRC_LOCAL)) {
        PR_INFO("usage: boiler <flow setpoint 20..80 degC> (used in manual mode)");
    }
}

static void cmd_mode(int argc, char *argv[])
{
    for (uint8_t m = 0; argc == 2 && m < HEAT_MODE_COUNT; m++) {
        if (!strcmp(argv[1], s_modes[m])) {
            settings_set_mode(m, SETTINGS_SRC_LOCAL);
            return;
        }
    }
    PR_INFO("usage: mode <auto|manual|home|leave>");
}

static void cmd_limits(int argc, char *argv[])
{
    if (argc != 3 || !settings_set_lower(parse_x10(argv[1]), SETTINGS_SRC_LOCAL) ||
        !settings_set_upper(parse_x10(argv[2]), SETTINGS_SRC_LOCAL)) {
        PR_INFO("usage: limits <lower 5..15> <upper 25..35>");
    }
}

static void print_day(const settings_t *c, uint8_t d)
{
    char    line[96];
    int     n = snprintf(line, sizeof(line), "day %d:", d + 1);
    for (uint8_t i = 0; i < SCHED_POINTS; i++) {
        uint8_t h, m;
        int16_t t;
        schedule_get_point(c->schedule, d, i, &h, &m, &t);
        n += snprintf(line + n, sizeof(line) - n, "  %02d:%02d %d.%d", h, m, t / 10, t % 10);
    }
    PR_NOTICE("%s", line);
}

static void cmd_sched(int argc, char *argv[])
{
    settings_t c;
    settings_get(&c);

    if (argc == 1) {
        for (uint8_t d = 0; d < SCHED_DAYS; d++) {
            print_day(&c, d);
        }
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "default")) {
        schedule_default(c.schedule);
        settings_set_schedule(c.schedule, SCHED_BYTES, SETTINGS_SRC_LOCAL);
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "hex")) {
        for (uint8_t d = 0; d < SCHED_DAYS; d++) {
            char line[64];
            int  n = 0;
            for (int i = 0; i < SCHED_POINTS * 4; i++) {
                n += snprintf(line + n, sizeof(line) - n, "%02x%s", c.schedule[d * 16 + i], i % 4 == 3 ? " " : "");
            }
            PR_NOTICE("day %d: %s", d + 1, line);
        }
        return;
    }
    if (argc == 6 && !strcmp(argv[1], "set")) {
        int day = atoi(argv[2]), idx = atoi(argv[3]), h = 0, m = 0;
        if (day >= 1 && day <= 7 && idx >= 1 && idx <= 4 && sscanf(argv[4], "%d:%d", &h, &m) == 2) {
            schedule_set_point(c.schedule, (uint8_t)(day - 1), (uint8_t)(idx - 1), (uint8_t)h, (uint8_t)m,
                               parse_x10(argv[5]));
            if (settings_set_schedule(c.schedule, SCHED_BYTES, SETTINGS_SRC_LOCAL)) {
                print_day(&c, (uint8_t)(day - 1));
            }
            return;
        }
    }
    PR_INFO("usage: sched | sched hex | sched default | sched set <day 1-7> <point 1-4> <hh:mm> <degC>");
}

static void cmd_time(int argc, char *argv[])
{
    if (argc == 2) {
        clock_set_utc((uint32_t)strtoul(argv[1], NULL, 10));
    }
    uint8_t  wd;
    uint16_t mn;
    if (clock_now(&wd, &mn)) {
        PR_NOTICE("local time: day %d %02d:%02d", wd + 1, mn / 60, mn % 60);
    } else {
        PR_NOTICE("time unknown; set with: time <UTC epoch seconds>");
    }
}

static void cmd_sensor(int argc, char *argv[])
{
    if (argc == 3 && !strcmp(argv[1], "sel")) {
        uint8_t sel = !strcmp(argv[2], "min") ? SENSOR_SEL_MIN
                      : !strcmp(argv[2], "avg") ? SENSOR_SEL_AVG
                                                : (uint8_t)(atoi(argv[2]) - 1);
        if (!settings_set_control_sensor(sel, SETTINGS_SRC_LOCAL)) {
            PR_INFO("usage: sensor sel <1-4|min|avg>");
        }
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "pull")) {
        ds18b20_set_pull(!strcmp(argv[2], "on")    ? DS18B20_PULL_ON
                         : !strcmp(argv[2], "off") ? DS18B20_PULL_OFF
                                                   : DS18B20_PULL_AUTO);
        return;
    }
    if (argc == 3) {
        int slot = atoi(argv[1]);
        if (slot >= 1 && slot <= SENSOR_SLOTS) {
            sensors_set((uint8_t)(slot - 1), parse_x10(argv[2]));
            return;
        }
    }
    for (uint8_t i = 0; i < SENSOR_SLOTS; i++) {
        int16_t v;
        bool    ok = sensors_get(i, &v);
        PR_NOTICE("sensor %d: %s%d", i + 1, ok ? "" : "stale/none ", ok ? v : 0);
    }
    ds18b20_print();
    bmp180_print();
    PR_INFO("usage: sensor | sensor <1-4> <degC> | sensor sel <1-4|min|avg> | sensor pull <auto|on|off>");
}

static void cmd_ot(int argc, char *argv[])
{
    if (argc == 3 && !strcmp(argv[1], "log")) {
        boiler_set_trace(!strcmp(argv[2], "on"));
        return;
    }
    if (argc == 5 && !strcmp(argv[1], "raw")) {
        uint32_t resp = 0;
        int rc = boiler_raw((uint8_t)atoi(argv[2]), (uint8_t)atoi(argv[3]), (uint16_t)strtoul(argv[4], NULL, 16),
                            &resp);
        PR_NOTICE("ot raw rc=%d resp=%08x %s id=%u val=%04x", rc, resp, ot_msg_type_str(ot_frame_type(resp)),
                  ot_frame_id(resp), ot_frame_value(resp));
        return;
    }
    PR_INFO("usage: ot log <on|off> | ot raw <type 0=read 1=write> <id> <hex value>");
}

static void cmd_sim(int argc, char *argv[])
{
    if (argc >= 2 && !strcmp(argv[1], "start")) {
        /* The emulator drives the OT RX pin as an output. On an assembled
         * adapter board that pin is driven by optocoupler U2 via 100 ohm. */
        if (argc != 3 || strcmp(argv[2], "bare")) {
            PR_WARN("sim drives GPIO%d as an output: only on a bare module, NOT on the adapter board.",
                    OT_RX_GPIO);
            PR_WARN("confirm with: sim start bare");
            return;
        }
        boiler_emu_start();
    } else if (argc >= 2 && !strcmp(argv[1], "stop")) {
        boiler_emu_stop();
    } else if (argc == 3 && !strcmp(argv[1], "speed")) {
        boiler_emu_set_speed((uint32_t)atoi(argv[2]));
    } else if (argc == 3 && !strcmp(argv[1], "fault")) {
        boiler_emu_set_fault((uint8_t)atoi(argv[2]));
    } else if (argc == 3 && !strcmp(argv[1], "mute")) {
        boiler_emu_set_mute(!strcmp(argv[2], "on"));
    } else if (argc == 3 && !strcmp(argv[1], "out")) {
        boiler_emu_set_outside(parse_x10(argv[2]));
    } else if (argc == 3 && !strcmp(argv[1], "room")) {
        boiler_emu_set_room(parse_x10(argv[2]));
    } else if (argc != 1) {
        PR_INFO("usage: sim [start bare|stop|speed N|fault <oem code, 0=clear>|mute on|off|out <degC>|room <degC>]");
        return;
    }
    boiler_emu_print();
}

static void cmd_led(int argc, char *argv[])
{
    status_led_test();
}

static void cmd_wdt(int argc, char *argv[])
{
    if (argc == 3 && !strcmp(argv[1], "stall")) {
        if (!strcmp(argv[2], "boiler")) {
            supervisor_simulate_stall(SUP_BOILER);
            return;
        }
        if (!strcmp(argv[2], "regulator")) {
            supervisor_simulate_stall(SUP_REGULATOR);
            return;
        }
    }
    PR_INFO("usage: wdt stall <boiler|regulator>  (the board resets ~20 s later)");
}

static void cmd_energy(int argc, char *argv[])
{
    if (argc == 2 && !strcmp(argv[1], "reset")) {
        energy_reset();
    }
    uint32_t c = energy_total_centi_kwh();
    PR_NOTICE("energy: power=%u W total=%u.%02u kWh (estimate)", energy_power_w(), c / 100, c % 100);
}

static void cmd_report(int argc, char *argv[])
{
    cloud_report_all();
    PR_NOTICE("report: everything queued for ThingsBoard (sent when online)");
}

static void cmd_wifi(int argc, char *argv[])
{
    if (argc == 2 && !strcmp(argv[1], "setup")) {
        net_setup_start();
        return;
    }
    /* an open network has no password */
    if ((argc == 2 || argc == 3) && net_set_wifi(argv[1], argc == 3 ? argv[2] : "")) {
        return;
    }
    char ssid[33];
    net_get_ssid(ssid, sizeof(ssid));
    PR_NOTICE("wifi: %s up=%d setup_ap=%d", ssid[0] ? ssid : "-", net_is_up(), net_setup_active());
    PR_INFO("usage: wifi <ssid> [password] | wifi setup  (names with spaces: use the web UI)");
}

static void cmd_tb(int argc, char *argv[])
{
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "host")) {
        cloud_set_server(argv[2], (uint16_t)(argc == 4 ? atoi(argv[3]) : 8883));
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "token")) {
        cloud_set_token(argv[2]);
        return;
    }
    if (argc == 3 && !strcmp(argv[1], "pass")) {
        cloud_set_password(strcmp(argv[2], "-") ? argv[2] : "");
        return;
    }
    char     host[64];
    uint16_t port;
    bool     tok;
    cloud_get_server(host, sizeof(host), &port, &tok);
    mqtt_port_stats_t st;
    mqtt_port_get_stats(&st);
    PR_NOTICE("tb: %s:%u token=%s pass=%s online=%d connects=%u sent=%u msgs/%u B received=%u msgs/%u B (payload only)",
              host[0] ? host : "-", port, tok ? "set" : "NONE", cloud_has_password() ? "set" : "none",
              cloud_is_online(), st.connects, st.tx_msgs, st.tx_bytes,
              st.rx_msgs, st.rx_bytes);
    char fw[192];
    fw_update_describe(fw, sizeof(fw));
    PR_NOTICE("tb: %s", fw);
    PR_INFO("usage: tb | tb host <name> [port: 8883 TLS, 1883 plain] | tb token <device access token> | "
            "tb pass <MQTT password of an own broker, - to clear>");
}

static void cmd_tz(int argc, char *argv[])
{
    if (argc == 2 && !settings_set_tz((int16_t)atoi(argv[1]), SETTINGS_SRC_LOCAL)) {
        PR_INFO("usage: tz [minutes from UTC, e.g. 180 for Moscow]");
        return;
    }
    settings_t c;
    settings_get(&c);
    PR_NOTICE("tz: %d min", c.tz_min);
}

static void cmd_factory(int argc, char *argv[])
{
    settings_reset();
}

static cli_cmd_t s_cli_cmd[] = {
    {.name = "status", .func = cmd_status, .help = "show everything"},
    {.name = "heat", .func = cmd_heat, .help = "heat <on|off>"},
    {.name = "set", .func = cmd_set, .help = "set <degC>: room setpoint"},
    {.name = "mode", .func = cmd_mode, .help = "mode <auto|manual|home|leave>"},
    {.name = "boiler", .func = cmd_boiler, .help = "boiler <degC>: flow setpoint for manual mode"},
    {.name = "limits", .func = cmd_limits, .help = "limits <lower> <upper>"},
    {.name = "sched", .func = cmd_sched, .help = "weekly program"},
    {.name = "time", .func = cmd_time, .help = "time [UTC epoch]"},
    {.name = "sensor", .func = cmd_sensor, .help = "room sensors"},
    {.name = "ot", .func = cmd_ot, .help = "OpenTherm trace / raw exchange"},
    {.name = "sim", .func = cmd_sim, .help = "boiler emulator on the OpenTherm loopback"},
    {.name = "led", .func = cmd_led, .help = "LED colour test"},
    {.name = "wdt", .func = cmd_wdt, .help = "watchdog test"},
    {.name = "energy", .func = cmd_energy, .help = "energy [reset]: estimated power/energy"},
    {.name = "tz", .func = cmd_tz, .help = "tz [minutes]: time zone"},
    {.name = "wifi", .func = cmd_wifi, .help = "wifi <ssid> [password] | wifi setup"},
    {.name = "tb", .func = cmd_tb, .help = "ThingsBoard server and token"},
    {.name = "report", .func = cmd_report, .help = "send everything to ThingsBoard again"},
    {.name = "factory", .func = cmd_factory, .help = "reset heating settings to defaults"},
    {.name = "kv", .func = tal_kv_cmd, .help = "kv store"},
};

/* Runs one command line from the LAN (POST /cli) or ThingsBoard ("cli" RPC)
 * against the table above. Lines with secrets are not echoed to the log. */
int app_cli_exec(const char *line)
{
    char  buf[128];
    char *argv[8];
    char *save = NULL;
    int   argc = 0;
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    static const char *const sep = " \t\r\n";
    for (char *tok = strtok_r(buf, sep, &save); tok && argc < 8; tok = strtok_r(NULL, sep, &save)) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        return -1;
    }
    bool secret = (!strcmp(argv[0], "wifi") && argc > 2) ||
                  (!strcmp(argv[0], "tb") && argc > 1 && (!strcmp(argv[1], "token") || !strcmp(argv[1], "pass")));
    for (size_t i = 0; i < sizeof(s_cli_cmd) / sizeof(s_cli_cmd[0]); i++) {
        if (!strcmp(argv[0], s_cli_cmd[i].name)) {
            PR_NOTICE("remote cli> %s", secret ? "(line with a secret, not shown)" : line);
            s_cli_cmd[i].func(argc, argv);
            return 0;
        }
    }
    return -1;
}

void app_cli_init(void)
{
    tal_cli_cmd_register(s_cli_cmd, sizeof(s_cli_cmd) / sizeof(s_cli_cmd[0]));
}
