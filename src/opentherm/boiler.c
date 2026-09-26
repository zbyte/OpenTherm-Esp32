/**
 * @file boiler.c
 * @brief OpenTherm master: polls the boiler once a second on the protocol core.
 *
 * Each cycle: ID0 status (with CH enable), ID1 control setpoint, ID25 flow
 * temperature, plus one rotating extra (modulation, fault flags, return temp).
 * The spec requires at least one exchange per second; the boiler falls back
 * to its own behaviour when communication stops.
 */

#include "boiler.h"

#include "tal_api.h"
#include "tuya_kconfig.h"

#include "ot_frame.h"
#include "ot_port.h"
#include "supervisor.h"

#define CYCLE_MS           1000
#define RESPONSE_TIMEOUT_MS 800
#define COMM_LOST_AFTER    5 /* consecutive failed status exchanges */

#define OT_CORE      1
#define OT_TASK_PRIO 20

static MUTEX_HANDLE    s_lock; /* status + demand */
static MUTEX_HANDLE    s_bus;  /* one exchange at a time (task vs. CLI) */
static boiler_status_t s_st;
static bool            s_trace;
static uint32_t        s_fail_streak;

static int exchange(ot_msg_type_e type, uint8_t id, uint16_t value, uint32_t *resp)
{
    uint32_t req = ot_frame_build(type, id, value);

    tal_mutex_lock(s_bus);
    int rc = ot_port_transceive(req, resp, RESPONSE_TIMEOUT_MS);
    tal_mutex_unlock(s_bus);

    if (rc == OT_PORT_OK && !ot_frame_parity_ok(*resp)) {
        rc = OT_PORT_INVALID;
    }
    if (rc == OT_PORT_OK && ot_frame_id(*resp) != id) {
        rc = OT_PORT_INVALID;
    }

    if (s_trace) {
        if (rc == OT_PORT_OK) {
            PR_DEBUG("ot > %08x %s id=%u val=%04x | < %08x %s val=%04x", req, ot_msg_type_str(type), id, value,
                     *resp, ot_msg_type_str(ot_frame_type(*resp)), ot_frame_value(*resp));
        } else {
            PR_DEBUG("ot > %08x %s id=%u val=%04x | rc=%d", req, ot_msg_type_str(type), id, value, rc);
        }
    }
    return rc;
}

/* Returns the data value of an ACK, or -1. */
static int32_t read_value(uint8_t id)
{
    uint32_t resp;
    if (exchange(OT_READ_DATA, id, 0, &resp) != OT_PORT_OK || ot_frame_type(resp) != OT_READ_ACK) {
        return -1;
    }
    return ot_frame_value(resp);
}

static void poll_cycle(uint32_t n)
{
    tal_mutex_lock(s_lock);
    bool    ch   = s_st.ch_enable_cmd;
    int16_t tset = s_st.tset_x10_cmd;
    tal_mutex_unlock(s_lock);

    uint32_t resp;
    uint16_t master = ch ? OT_MASTER_CH_ENABLE : 0;
    int      rc     = exchange(OT_READ_DATA, OT_ID_STATUS, (uint16_t)(master << 8), &resp);
    bool     ok     = rc == OT_PORT_OK && ot_frame_type(resp) == OT_READ_ACK;

    tal_mutex_lock(s_lock);
    bool fault = false;
    if (ok) {
        uint8_t slave  = (uint8_t)ot_frame_value(resp);
        fault          = slave & OT_SLAVE_FAULT;
        s_st.fault     = slave & OT_SLAVE_FAULT;
        s_st.ch_active = slave & OT_SLAVE_CH_ACTIVE;
        s_st.flame     = slave & OT_SLAVE_FLAME;
        if (!s_st.comm_ok) {
            PR_NOTICE("boiler: communication OK");
        }
        s_st.comm_ok  = true;
        s_fail_streak = 0;
    } else {
        s_st.comm_errors++;
        if (++s_fail_streak == COMM_LOST_AFTER) {
            PR_ERR("boiler: communication lost (rc=%d)", rc);
            s_st.comm_ok    = false;
            s_st.flow_valid = false;
            s_st.mod_valid  = false;
            s_st.flame      = false;
            s_st.ch_active  = false;
        }
    }
    tal_mutex_unlock(s_lock);

    if (!ok) {
        return; /* no point sending more while the boiler does not answer */
    }

    exchange(OT_WRITE_DATA, OT_ID_TSET, ot_x10_to_f88(tset), &resp);

    int32_t v = read_value(OT_ID_TBOILER);
    tal_mutex_lock(s_lock);
    s_st.flow_valid = v >= 0;
    if (v >= 0) {
        s_st.flow_x10 = ot_f88_to_x10((uint16_t)v);
    }
    tal_mutex_unlock(s_lock);

    switch (n % 4) {
    case 0:
        v = read_value(OT_ID_REL_MOD);
        tal_mutex_lock(s_lock);
        s_st.mod_valid = v >= 0;
        if (v >= 0) {
            s_st.rel_mod_x10 = ot_f88_to_x10((uint16_t)v);
        }
        tal_mutex_unlock(s_lock);
        break;
    case 2:
        if (fault || n % 20 == 2) {
            v = read_value(OT_ID_ASF_FLAGS);
            if (v >= 0) {
                tal_mutex_lock(s_lock);
                s_st.asf_flags = (uint8_t)(v >> 8);
                s_st.oem_fault = (uint8_t)v;
                tal_mutex_unlock(s_lock);
            }
        }
        break;
    default:
        break;
    }
}

static void boiler_task(void *arg)
{
    /* Init here, not in boiler_init(): the GPIO ISR service is allocated on
     * the calling core, and RX edges must be timestamped on the OT core. */
    ot_port_cfg_t cfg = {
        .tx_gpio = OT_TX_GPIO,
        .rx_gpio = OT_RX_GPIO,
#if defined(OT_TX_ACTIVE_LOW) && OT_TX_ACTIVE_LOW
        .tx_active_low = true,
#endif
#if defined(OT_RX_ACTIVE_LOW) && OT_RX_ACTIVE_LOW
        .rx_active_low = true,
#endif
    };
    if (ot_port_init(&cfg) != OT_PORT_OK) {
        PR_ERR("boiler: OpenTherm port init failed");
        for (;;) {
            tal_system_sleep(60000);
        }
    }
    PR_NOTICE("boiler: OpenTherm on TX=GPIO%d RX=GPIO%d", cfg.tx_gpio, cfg.rx_gpio);

    uint32_t n = 0;
    int64_t  next = ot_port_time_us();

    for (;;) {
        poll_cycle(n++);
        supervisor_beat(SUP_BOILER);
        next += CYCLE_MS * 1000LL;
        int64_t wait_us = next - ot_port_time_us();
        if (wait_us < 0) {
            next    = ot_port_time_us();
            wait_us = 0;
        }
        tal_system_sleep((uint32_t)(wait_us / 1000));
    }
}

void boiler_init(void)
{
    tal_mutex_create_init(&s_lock);
    tal_mutex_create_init(&s_bus);
    s_st.tset_x10_cmd = HEAT_FLOW_MIN_X10;

    if (ot_port_task_create(boiler_task, "ot_master", 4096, OT_TASK_PRIO, OT_CORE, NULL) != OT_PORT_OK) {
        PR_ERR("boiler: task create failed");
    }
}

void boiler_set_demand(bool ch_enable, int16_t tset_x10)
{
    tal_mutex_lock(s_lock);
    s_st.ch_enable_cmd = ch_enable;
    s_st.tset_x10_cmd  = tset_x10;
    tal_mutex_unlock(s_lock);
}

void boiler_get_status(boiler_status_t *out)
{
    tal_mutex_lock(s_lock);
    *out = s_st;
    tal_mutex_unlock(s_lock);
}

void boiler_set_trace(bool on)
{
    s_trace = on;
}

int boiler_raw(uint8_t type, uint8_t id, uint16_t value, uint32_t *response)
{
    return exchange((ot_msg_type_e)type, id, value, response);
}
