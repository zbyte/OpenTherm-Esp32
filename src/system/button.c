/**
 * @file button.c
 * @brief Service button (BOOT, GPIO0): actions on RELEASE, by hold time.
 */

#include "button.h"

#include "tal_api.h"
#include "tuya_kconfig.h"

#include "net.h"
#include "ot_port.h"
#include "settings.h"

#define POLL_MS       20
#define DEBOUNCE_MS   60
#define SETUP_MS      5000
#define FACTORY_MS    10000
#define CORE          0
#define PRIO          6

static volatile button_hold_e s_level;

static button_hold_e level_for(uint32_t held_ms)
{
    return held_ms >= FACTORY_MS ? BUTTON_HOLD_FACTORY
           : held_ms >= SETUP_MS  ? BUTTON_HOLD_SETUP
                                  : BUTTON_HOLD_NONE;
}

static void on_release(uint32_t held_ms)
{
    switch (level_for(held_ms)) {
    case BUTTON_HOLD_SETUP:
        PR_NOTICE("button: %u ms -> setup access point", held_ms);
        net_setup_start();
        break;
    case BUTTON_HOLD_FACTORY:
        PR_NOTICE("button: %u ms -> heating settings reset", held_ms);
        settings_reset();
        break;
    default:
        PR_INFO("button: short press (%u ms), no action", held_ms);
        break;
    }
}

static void button_task(void *arg)
{
    bool     pressed = false;
    uint32_t edge_ms = 0, down_ms = 0;

    /* Held at boot (e.g. still held after a reset): wait for a release. */
    while (btn_port_pressed(BUTTON_GPIO)) {
        tal_system_sleep(POLL_MS);
    }

    for (;;) {
        bool     raw = btn_port_pressed(BUTTON_GPIO);
        uint32_t now = tal_system_get_millisecond();

        if (raw != pressed) {
            if (!edge_ms) {
                edge_ms = now;
            } else if (now - edge_ms >= DEBOUNCE_MS) {
                pressed = raw;
                edge_ms = 0;
                if (pressed) {
                    down_ms = now;
                } else {
                    s_level = BUTTON_HOLD_NONE;
                    on_release(now - down_ms);
                }
            }
        } else {
            edge_ms = 0;
        }

        if (pressed) {
            button_hold_e lvl = level_for(now - down_ms);
            if (lvl != s_level) {
                s_level = lvl;
                PR_INFO("button: release now to %s", lvl == BUTTON_HOLD_FACTORY ? "reset heating settings"
                                                                                  : "open the setup access point");
            }
        }
        tal_system_sleep(POLL_MS);
    }
}

void button_init(void)
{
    if (btn_port_init(BUTTON_GPIO) != OT_PORT_OK ||
        ot_port_task_create(button_task, "button", 3072, PRIO, CORE, NULL) != OT_PORT_OK) {
        PR_ERR("button: init failed");
    }
}

button_hold_e button_hold_level(void)
{
    return s_level;
}
