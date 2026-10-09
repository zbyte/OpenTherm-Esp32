/**
 * @file supervisor.c
 * @brief Watchdog supervisor: the hardware task watchdog is fed only while
 *        every control task keeps beating; also confirms a fresh OTA image.
 *
 * The cloud thread is deliberately not supervised: a slow or hung network
 * must never reboot the heating control. A stuck OpenTherm master or
 * regulator stops the feeding and the task watchdog (10 s) resets the chip.
 *
 * After an OTA update the image boots in "pending verify"; it is confirmed
 * once the control tasks have run healthy for OTA_CONFIRM_MS. A crash or
 * watchdog reset before that makes the bootloader roll back.
 */

#include "supervisor.h"

#include <stdio.h>

#include "tal_api.h"

#include "cloud.h"
#include "ot_port.h"

#define CHECK_MS       1000
#define STALE_MS       15000
#define BOOT_GRACE_MS  30000
#define OTA_CONFIRM_MS 60000
#define OTA_CLOUD_DEADLINE_MS (5 * 60 * 1000)
#define WDT_TIMEOUT_MS 10000
#define CORE           0
#define PRIO           15

static const char *const s_names[SUP_COUNT] = {"boiler", "regulator"};
static volatile uint32_t s_beat_ms[SUP_COUNT];
static volatile int      s_stalled = -1;

void supervisor_beat(sup_task_e task)
{
    if (task < SUP_COUNT && (int)task != s_stalled) {
        s_beat_ms[task] = tal_system_get_millisecond();
    }
}

void supervisor_simulate_stall(sup_task_e task)
{
    PR_WARN("supervisor: simulating a stalled %s task", s_names[task]);
    s_stalled = task;
}

static char s_last_reset[64] = "none";

const char *supervisor_last_stall(void)
{
    return s_last_reset;
}

static bool all_alive(uint32_t now, bool report)
{
    bool ok = true;
    for (int i = 0; i < SUP_COUNT; i++) {
        uint32_t beat = s_beat_ms[i];
        bool     stale = beat ? (now - beat > STALE_MS) : (now > BOOT_GRACE_MS);
        if (stale) {
            ok = false;
            if (report) {
                PR_ERR("supervisor: %s task stalled (last beat %u ms ago), watchdog will reset", s_names[i],
                       beat ? now - beat : now);
                /* survives the reset: shown by the next boot and `status` */
                sys_port_postmortem_set((uint32_t)(i + 1), now / 1000);
            }
        }
    }
    return ok;
}

static void supervisor_task(void *arg)
{
    bool ota_pending = sys_port_ota_pending_verify();
    bool reported    = false;

    if (wdt_port_watch_self(WDT_TIMEOUT_MS) != OT_PORT_OK) {
        PR_ERR("supervisor: cannot subscribe to the task watchdog");
    }
    if (ota_pending) {
        PR_WARN("supervisor: new firmware on %s awaiting verification", sys_port_running_partition());
    }

    uint32_t healthy_since = tal_system_get_millisecond();
    for (;;) {
        uint32_t now = tal_system_get_millisecond();
        if (all_alive(now, !reported)) {
            wdt_port_feed();
            reported = false;
            /* An OTA image must also prove it can reach ThingsBoard:
             * otherwise it could never be managed remotely again. */
            if (ota_pending && now - healthy_since >= OTA_CONFIRM_MS && cloud_state() == CLOUD_ONLINE) {
                ota_pending = sys_port_ota_mark_valid() != OT_PORT_OK;
                PR_NOTICE("supervisor: firmware confirmed%s", ota_pending ? " FAILED" : "");
            } else if (ota_pending && now >= OTA_CLOUD_DEADLINE_MS) {
                PR_ERR("supervisor: new firmware did not reach the cloud, rolling back");
                sys_port_ota_rollback();
                ota_pending = false; /* no previous image: keep running */
            }
        } else {
            /* keep not feeding: the watchdog resets the chip */
            reported      = true;
            healthy_since = now;
        }
        tal_system_sleep(CHECK_MS);
    }
}

void supervisor_init(void)
{
    PR_NOTICE("supervisor: reset reason: %s, running from %s", sys_port_reset_reason(),
              sys_port_running_partition());

    uint32_t code, uptime_s;
    if (sys_port_postmortem_take(&code, &uptime_s) && code >= 1 && code <= SUP_COUNT) {
        snprintf(s_last_reset, sizeof(s_last_reset), "%s task stalled at uptime %us", s_names[code - 1],
                 (unsigned)uptime_s);
        PR_WARN("supervisor: previous watchdog reset: %s", s_last_reset);
    }

    if (ot_port_task_create(supervisor_task, "supervisor", 3072, PRIO, CORE, NULL) != OT_PORT_OK) {
        PR_ERR("supervisor: task create failed");
    }
}
