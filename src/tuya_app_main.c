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

#include "boiler.h"
#include "clock.h"
#include "cloud.h"
#include "regulator.h"
#include "sensors.h"
#include "settings.h"

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

extern void app_cli_init(void);

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
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

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

    /* Control first: it must run whether or not the cloud ever comes up. */
    clock_init();
    sensors_init();
    settings_init(on_settings_change);
    boiler_init();
    regulator_init(on_regulator_change);

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
