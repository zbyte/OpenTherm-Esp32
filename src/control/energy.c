/**
 * @file energy.c
 * @brief Estimated boiler power and energy from OpenTherm data.
 */

#include "energy.h"

#include <string.h>

#include "tal_api.h"
#include "tuya_kconfig.h"

#include "ot_port.h"

#define KV_KEY       "energy_wh"
#define SAVE_EVERY_S (15 * 60)

static MUTEX_HANDLE s_lock;
static double       s_wh;      /* running total, Wh */
static double       s_saved_wh;
static uint32_t     s_power_w;
static float        s_since_save_s;

static void kv_save(void)
{
    uint64_t wh = (uint64_t)s_wh;
    if (tal_kv_set(KV_KEY, (const uint8_t *)&wh, sizeof(wh)) == OPRT_OK) {
        s_saved_wh = s_wh;
    }
}

void energy_init(void)
{
    tal_mutex_create_init(&s_lock);

    uint64_t kv_wh = 0;
    uint8_t *buf   = NULL;
    size_t   len   = 0;
    if (tal_kv_get(KV_KEY, &buf, &len) == OPRT_OK && len == sizeof(kv_wh)) {
        memcpy(&kv_wh, buf, sizeof(kv_wh));
    }
    if (buf) {
        tal_kv_free(buf);
    }

    /* RTC memory holds the exact value across resets; after a power loss
     * only the KV copy (at most 15 min old) is left. Take the larger. */
    uint64_t rtc_wh = 0;
    bool     rtc_ok = sys_port_energy_get(&rtc_wh);
    s_wh            = (double)(rtc_ok && rtc_wh > kv_wh ? rtc_wh : kv_wh);
    s_saved_wh      = (double)kv_wh;
    PR_NOTICE("energy: total %.2f kWh (%s)", s_wh / 1000.0, rtc_ok && rtc_wh > kv_wh ? "rtc" : "kv");
}

void energy_tick(float dt_s, const boiler_status_t *b)
{
    uint32_t p = 0;
    if (b->comm_ok) {
        if (b->mod_valid) {
            int32_t mod = b->rel_mod_x10 < 0 ? 0 : (b->rel_mod_x10 > 1000 ? 1000 : b->rel_mod_x10);
            p           = (uint32_t)((int64_t)BOILER_POWER_W * mod / 1000);
        } else if (b->flame) {
            p = BOILER_POWER_W; /* no modulation data: upper estimate */
        }
    }

    tal_mutex_lock(s_lock);
    s_power_w = p;
    s_wh += p * dt_s / 3600.0;
    sys_port_energy_set((uint64_t)s_wh);
    s_since_save_s += dt_s;
    bool save = s_since_save_s >= SAVE_EVERY_S && (uint64_t)s_wh != (uint64_t)s_saved_wh;
    if (s_since_save_s >= SAVE_EVERY_S) {
        s_since_save_s = 0;
    }
    if (save) {
        kv_save();
    }
    tal_mutex_unlock(s_lock);
}

uint32_t energy_power_w(void)
{
    return s_power_w;
}

uint32_t energy_total_centi_kwh(void)
{
    tal_mutex_lock(s_lock);
    uint32_t v = (uint32_t)(s_wh / 10.0);
    tal_mutex_unlock(s_lock);
    return v;
}

void energy_reset(void)
{
    tal_mutex_lock(s_lock);
    s_wh = 0;
    sys_port_energy_set(0);
    kv_save();
    tal_mutex_unlock(s_lock);
    PR_NOTICE("energy: counter reset");
}
