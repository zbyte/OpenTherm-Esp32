/**
 * @file status_led.c
 * @brief On-board WS2812 status LED.
 */

#include "status_led.h"

#include "tal_api.h"
#include "tuya_kconfig.h"

#include "boiler.h"
#include "button.h"
#include "cloud.h"
#include "ot_port.h"
#include "regulator.h"

#define TICK_MS 50
#define CORE    0
#define PRIO    4
#define LEVEL   24 /* of 255: an indicator, not a lamp */

typedef struct {
    uint8_t  r, g, b;
    uint16_t on_ms;  /* 0 = steady */
    uint16_t period_ms;
} pattern_t;

static const pattern_t P_FACTORY = {LEVEL, 0, LEVEL, 0, 0};
static const pattern_t P_UNBIND  = {0, LEVEL, LEVEL, 0, 0};
static const pattern_t P_FAULT   = {LEVEL, 0, 0, 500, 1000};
static const pattern_t P_PAIRING = {0, 0, LEVEL, 200, 400};
static const pattern_t P_OFFLINE = {LEVEL, LEVEL / 2, 0, 250, 2000};
static const pattern_t P_HEATING = {LEVEL, LEVEL / 4, 0, 0, 0};
static const pattern_t P_ONLINE  = {0, LEVEL, 0, 0, 0};

static const pattern_t *select_pattern(void)
{
    switch (button_hold_level()) {
    case BUTTON_HOLD_FACTORY:
        return &P_FACTORY;
    case BUTTON_HOLD_UNBIND:
        return &P_UNBIND;
    default:
        break;
    }

    regulator_status_t r;
    regulator_get_status(&r);
    if (r.faults) {
        return &P_FAULT;
    }

    switch (cloud_state()) {
    case CLOUD_PAIRING:
        return &P_PAIRING;
    case CLOUD_OFFLINE:
        return &P_OFFLINE;
    default:
        break;
    }

    boiler_status_t b;
    boiler_get_status(&b);
    return b.flame ? &P_HEATING : &P_ONLINE;
}

static volatile bool s_test;

void status_led_test(void)
{
    s_test = true;
}

/* Lights each byte on the wire alone (3 s, then 1 s dark), so the channel
 * order can be read off the LED regardless of the configured order. */
static void run_test(void)
{
    const uint8_t v = 64;
    for (int cycle = 0; cycle < 5; cycle++) {
        for (int i = 0; i < 3; i++) {
            PR_NOTICE("status_led: test byte %d", i);
            led_port_set_raw(i == 0 ? v : 0, i == 1 ? v : 0, i == 2 ? v : 0);
            tal_system_sleep(3000);
            led_port_set_raw(0, 0, 0);
            tal_system_sleep(1000);
        }
        tal_system_sleep(3000);
    }
    PR_NOTICE("status_led: test done");
    s_test = false;
}

static void led_task(void *arg)
{
    const pattern_t *last = NULL;
    bool             last_on = false;

    for (;;) {
        if (s_test) {
            run_test();
            last = NULL;
        }
        const pattern_t *p   = select_pattern();
        uint32_t         now = tal_system_get_millisecond();
        bool on = p->on_ms == 0 || (now % p->period_ms) < p->on_ms;

        if (p != last || on != last_on) {
            if (on) {
                led_port_set(p->r, p->g, p->b);
            } else {
                led_port_set(0, 0, 0);
            }
            last    = p;
            last_on = on;
        }
        tal_system_sleep(TICK_MS);
    }
}

void status_led_init(void)
{
#if defined(LED_ORDER_GRB) && LED_ORDER_GRB
    bool grb = true;
#else
    bool grb = false;
#endif
    if (led_port_init(LED_GPIO, grb) != OT_PORT_OK ||
        ot_port_task_create(led_task, "status_led", 3072, PRIO, CORE, NULL) != OT_PORT_OK) {
        PR_ERR("status_led: init failed");
    }
}
