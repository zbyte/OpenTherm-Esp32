/**
 * @file tuya_app_main.c
 * @brief Entry point: platform services, heating state, cloud.
 */

#include "cJSON.h"
#include "tal_api.h"
#include "tal_cli.h"
#include "tkl_output.h"

#include "cloud.h"
#include "heating_state.h"

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

extern void app_cli_init(void);

static void on_heating_change(heating_src_e src)
{
    (void)src;
    cloud_report_all();
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

    heating_state_init(on_heating_change);

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
        .stackDepth = 1024 * 4,
        .priority   = THREAD_PRIO_1,
        .thrdname   = "tuya_app_main",
    };
    tal_thread_create_and_start(&s_app_thread, NULL, NULL, app_thread, NULL, &cfg);
}
