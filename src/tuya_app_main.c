/**
 * @file tuya_app_main.c
 * @brief Entry point: platform services, settings, control loop, cloud.
 *
 * Cores: OpenTherm master and regulator are pinned to core 1 so network
 * latency on core 0 cannot break protocol timing; the Tuya client runs in
 * this thread.
 */

#include "cJSON.h"
#include "tal_api.h"
#include "tal_cli.h"
#include "tkl_output.h"

#include "bmp180.h"
#include "boiler.h"
#include "button.h"
#include "clock.h"
#include "energy.h"
#include "log_ring.h"
#include "ot_port.h"
#include "status_led.h"
#include "supervisor.h"
#include "cloud.h"
#include "ds18b20.h"
#include "regulator.h"
#include "sensors.h"
#include "settings.h"

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

extern void app_cli_init(void);
extern int  app_cli_exec(const char *line);

static void on_settings_change(uint32_t fields, settings_src_e src)
{
    cloud_report_settings(fields ? fields : 0xffffffff);
}

static void on_regulator_change(uint32_t fields)
{
    cloud_report_status(fields);
}

static void user_main(void)
{
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = tal_malloc, .free_fn = tal_free});
    /* Before the log: every log line timestamps itself through the time
     * service, whose mutex this creates. */
    tal_time_service_init();
    log_ring_init();
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)log_ring_output);

    PR_NOTICE("%s %s, TuyaOpen %s, %s/%s", PROJECT_NAME, PROJECT_VERSION, OPEN_VERSION, PLATFORM_CHIP,
              PLATFORM_BOARD);

    /* KV encryption key/seed are fixed by the SDK demos; keep them so data
     * written by earlier builds (license, activation) stays readable. */
    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key  = "dflfuap134ddlduq",
    });
    tal_sw_timer_init();
    tal_workq_init();

    tal_cli_init();
    app_cli_init();
    lan_debug_set_handlers(log_ring_read, app_cli_exec);

    /* Control first: it must run whether or not the cloud ever comes up. */
    clock_init();
    sensors_init();
    ds18b20_init();
    bmp180_init();
    settings_init(on_settings_change);
    energy_init();
    boiler_init();
    regulator_init(on_regulator_change);
    supervisor_init();
    button_init();
    status_led_init();

    cloud_init();
    cloud_run();
}

static THREAD_HANDLE s_app_thread = NULL;

static void app_thread(void *arg)
{
    user_main();

    tal_thread_delete(s_app_thread);
    s_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T cfg = {
        .stackDepth = 1024 * 6,
        .priority   = THREAD_PRIO_1,
        .thrdname   = "tuya_app_main",
    };
    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &cfg);
}
