/**
 * @file heating_state.c
 * @brief Heating settings owned by the device, persisted in KV.
 */

#include "heating_state.h"

#include "tal_api.h"

#define KV_KEY_SWITCH "heat_sw"

/* Before the first pairing the device heats with defaults (spec). */
#define DEFAULT_SWITCH true

static MUTEX_HANDLE        s_mutex;
static bool                s_switch = DEFAULT_SWITCH;
static heating_change_cb_t s_on_change;

static void kv_load_bool(const char *key, bool *value)
{
    uint8_t *buf = NULL;
    size_t   len = 0;

    if (OPRT_OK == tal_kv_get(key, &buf, &len) && len == 1) {
        *value = buf[0] != 0;
    }
    if (buf) {
        tal_kv_free(buf);
    }
}

static void kv_save_bool(const char *key, bool value)
{
    uint8_t byte = value ? 1 : 0;

    if (OPRT_OK != tal_kv_set(key, &byte, 1)) {
        PR_ERR("kv save %s failed", key);
    }
}

void heating_state_init(heating_change_cb_t on_change)
{
    tal_mutex_create_init(&s_mutex);
    s_on_change = on_change;
    kv_load_bool(KV_KEY_SWITCH, &s_switch);
    PR_NOTICE("heating: switch=%d", s_switch);
}

bool heating_get_switch(void)
{
    tal_mutex_lock(s_mutex);
    bool on = s_switch;
    tal_mutex_unlock(s_mutex);
    return on;
}

void heating_set_switch(bool on, heating_src_e src)
{
    tal_mutex_lock(s_mutex);
    bool changed = s_switch != on;
    s_switch     = on;
    if (changed) {
        kv_save_bool(KV_KEY_SWITCH, on);
    }
    tal_mutex_unlock(s_mutex);

    PR_NOTICE("heating: switch=%d (%s%s)", on, src == HEATING_SRC_CLOUD ? "cloud" : "local",
              changed ? "" : ", unchanged");

    /* Report even when unchanged: the cloud expects an echo for every command. */
    if (s_on_change) {
        s_on_change(src);
    }
}
