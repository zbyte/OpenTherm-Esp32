/**
 * @file boiler_emu.c
 * @brief Emulated 18 kW electric boiler + house on the OpenTherm loopback.
 *
 * Lumped model, 1 s steps (times the speed factor):
 *   water:  C_w dTf/dt = P - UA_rad (Tf - Tr)
 *   house:  C_h dTr/dt = UA_rad (Tf - Tr) - UA_out (Tr - To)
 * Power comes in 3 kW element stages, the boiler's own control holds the flow
 * at the setpoint with a 2 K hysteresis like a staged electric boiler.
 */

#include "boiler_emu.h"

#include "tal_api.h"

#include "ot_frame.h"
#include "ot_port.h"
#include "sensors.h"

#define P_STAGE_KW  3.0f
#define N_STAGES    6 /* 18 kW */
#define C_WATER_KJ  209.0f   /* ~50 l system */
#define UA_RAD_KW   0.30f    /* kW/K radiators */
#define C_HOUSE_KJ  15000.0f /* kJ/K */
#define UA_OUT_KW   0.25f    /* kW/K envelope */

static MUTEX_HANDLE s_lock;
static TIMER_ID     s_timer;
static bool         s_running;
static uint32_t     s_speed = 1;
static bool         s_mute;
static uint8_t      s_fault_code;

static bool  s_ch_enable;
static float s_tset  = 10.0f;
static float s_flow  = 20.0f;
static float s_room  = 18.0f;
static float s_out   = -5.0f;
static int   s_stages;

static void model_step(TIMER_ID id, void *arg)
{
    tal_mutex_lock(s_lock);
    for (uint32_t i = 0; i < s_speed; i++) {
        if (!s_ch_enable || s_fault_code) {
            s_stages = 0;
        } else {
            /* staged control: add a stage below the band, drop one above */
            float err = s_tset - s_flow;
            if (err > 2.0f && s_stages < N_STAGES) {
                s_stages++;
            } else if (err < -1.0f && s_stages > 0) {
                s_stages--;
            } else if (err > 0.0f && s_stages == 0) {
                s_stages = 1;
            }
        }
        float p_kw   = s_stages * P_STAGE_KW;
        float q_rad  = UA_RAD_KW * (s_flow - s_room);
        float q_out  = UA_OUT_KW * (s_room - s_out);
        s_flow      += (p_kw - q_rad) / C_WATER_KJ;
        s_room      += (q_rad - q_out) / C_HOUSE_KJ;
    }
    int16_t room_x10 = (int16_t)(s_room * 10.0f);
    tal_mutex_unlock(s_lock);

    sensors_set(0, room_x10);
}

static uint32_t handle(uint32_t req)
{
    if (s_mute || !ot_frame_parity_ok(req)) {
        return 0;
    }

    ot_msg_type_e type = ot_frame_type(req);
    uint8_t       id   = ot_frame_id(req);
    uint16_t      val  = ot_frame_value(req);
    uint32_t      resp = 0;

    tal_mutex_lock(s_lock);
    if (type == OT_READ_DATA) {
        switch (id) {
        case OT_ID_STATUS: {
            s_ch_enable   = (val >> 8) & OT_MASTER_CH_ENABLE;
            uint8_t slave = 0;
            slave |= s_fault_code ? OT_SLAVE_FAULT : 0;
            slave |= s_ch_enable ? OT_SLAVE_CH_ACTIVE : 0;
            slave |= s_stages ? OT_SLAVE_FLAME : 0;
            resp = ot_frame_build(OT_READ_ACK, id, (uint16_t)((val & 0xff00) | slave));
        } break;
        case OT_ID_TBOILER:
            resp = ot_frame_build(OT_READ_ACK, id, ot_x10_to_f88((int16_t)(s_flow * 10.0f)));
            break;
        case OT_ID_TRET:
            resp = ot_frame_build(OT_READ_ACK, id, ot_x10_to_f88((int16_t)((s_flow - 5.0f) * 10.0f)));
            break;
        case OT_ID_REL_MOD:
            resp = ot_frame_build(OT_READ_ACK, id, ot_x10_to_f88((int16_t)(s_stages * 1000 / N_STAGES)));
            break;
        case OT_ID_ASF_FLAGS:
            resp = ot_frame_build(OT_READ_ACK, id, (uint16_t)((s_fault_code ? 0x01 : 0) << 8 | s_fault_code));
            break;
        case OT_ID_SLAVE_CONFIG:
            resp = ot_frame_build(OT_READ_ACK, id, 0x0000);
            break;
        default:
            resp = ot_frame_build(OT_UNKNOWN_DATAID, id, val);
            break;
        }
    } else if (type == OT_WRITE_DATA) {
        switch (id) {
        case OT_ID_TSET:
            s_tset = ot_f88_to_x10(val) / 10.0f;
            resp   = ot_frame_build(OT_WRITE_ACK, id, val);
            break;
        default:
            resp = ot_frame_build(OT_UNKNOWN_DATAID, id, val);
            break;
        }
    }
    tal_mutex_unlock(s_lock);
    return resp;
}

int boiler_emu_start(void)
{
    if (s_running) {
        return OPRT_OK;
    }
    if (!s_lock) {
        tal_mutex_create_init(&s_lock);
        tal_sw_timer_create(model_step, NULL, &s_timer);
    }
    /* Slave replies 20..800 ms after the request per spec; 30 ms here. */
    if (ot_port_emu_start(handle, 30) != OT_PORT_OK) {
        PR_ERR("emu: start failed (port not initialised?)");
        return OPRT_COM_ERROR;
    }
    tal_sw_timer_start(s_timer, 1000, TAL_TIMER_CYCLE);
    s_running = true;
    PR_NOTICE("emu: 18 kW electric boiler emulator running on the OpenTherm loopback");
    return OPRT_OK;
}

void boiler_emu_stop(void)
{
    if (!s_running) {
        return;
    }
    tal_sw_timer_stop(s_timer);
    ot_port_emu_stop();
    s_running = false;
    PR_NOTICE("emu: stopped");
}

bool boiler_emu_running(void)
{
    return s_running;
}

void boiler_emu_set_speed(uint32_t factor)
{
    s_speed = factor ? factor : 1;
}

void boiler_emu_set_fault(uint8_t oem_code)
{
    s_fault_code = oem_code;
}

void boiler_emu_set_mute(bool mute)
{
    s_mute = mute;
}

void boiler_emu_set_outside(int16_t x10)
{
    s_out = x10 / 10.0f;
}

void boiler_emu_set_room(int16_t x10)
{
    s_room = x10 / 10.0f;
}

void boiler_emu_print(void)
{
    if (!s_lock) {
        PR_NOTICE("emu: not started");
        return;
    }
    tal_mutex_lock(s_lock);
    PR_NOTICE("emu: run=%d x%u ch=%d tset=%d flow=%d room=%d out=%d stages=%d (%d kW) fault=%u mute=%d", s_running,
              s_speed, s_ch_enable, (int)(s_tset * 10), (int)(s_flow * 10), (int)(s_room * 10), (int)(s_out * 10),
              s_stages, (int)(s_stages * P_STAGE_KW), s_fault_code, s_mute);
    tal_mutex_unlock(s_lock);
}
