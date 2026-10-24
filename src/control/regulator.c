/**
 * @file regulator.c
 * @brief Heating control: boiler flow setpoint from the mode.
 *
 * auto   -> weekly program points are boiler FLOW setpoints (< 20 degC = off)
 * manual -> boiler_set (flow setpoint set by hand)
 * home   -> room held at temp_set, PI -> flow setpoint
 * leave  -> room held at the away temperature, PI -> flow setpoint
 * switch off -> room held at the frost temperature
 * The boiler stages its elements to hold the flow setpoint.
 *
 * Fail-safe (spec): no valid room temperature -> fixed moderate flow while
 * heating is switched on, never "heat at maximum". With heating switched off
 * and no sensor the boiler's own frost protection is relied on.
 */

#include "regulator.h"

#include "tal_api.h"
#include "tuya_kconfig.h"

#include "boiler.h"
#include "clock.h"
#include "energy.h"
#include "ot_port.h"
#include "sensors.h"
#include "settings.h"
#include "supervisor.h"

#define PERIOD_MS 2000
#define CORE      1
#define PRIO      10

/* PI tuning in degC: flow = target + FLOW_OFFSET + KP*err + I */
#define FLOW_OFFSET 10.0f
#define KP          8.0f
#define TI_S        1800.0f
#define I_MIN       -10.0f
#define I_MAX       35.0f
#define DEMAND_ON   0.2f /* err above -> heat */
#define DEMAND_OFF -0.3f /* err below -> stop */

static MUTEX_HANDLE          s_lock;
static regulator_status_t    s_st;
static regulator_change_cb_t s_on_change;
static float                 s_integral;

/* Room target for the room-regulated cases: switch off (frost), home, leave. */
static int16_t room_target(const settings_t *cfg, const char **src)
{
    if (!cfg->sw) {
        *src = "frost";
        return HEAT_FROST_X10;
    }
    if (cfg->mode == HEAT_MODE_LEAVE) {
        *src = "away";
        return HEAT_AWAY_X10;
    }
    *src = "temp_set";
    return cfg->temp_set_x10;
}

/*
 * Flow-driven modes (user's choice): the value IS the boiler flow setpoint.
 *   manual -> boiler_set
 *   auto   -> the current weekly-program point; a point below 20 degC means
 *             "boiler off" for that period; unknown time -> boiler_set
 * Returns false for room-regulated modes. *flow_x10 == 0 means off.
 */
static bool flow_setpoint(const settings_t *cfg, bool time_valid, int16_t *flow_x10, const char **src)
{
    if (!cfg->sw) {
        return false;
    }
    if (cfg->mode == HEAT_MODE_MANUAL) {
        *flow_x10 = cfg->boiler_set_x10;
        *src      = "manual boiler setpoint";
        return true;
    }
    if (cfg->mode != HEAT_MODE_AUTO) {
        return false;
    }

    uint8_t  wd;
    uint16_t minute;
    int16_t  v;
    if (!(time_valid && clock_now(&wd, &minute) && schedule_target(cfg->schedule, wd, minute, &v))) {
        *flow_x10 = cfg->boiler_set_x10;
        *src      = "boiler setpoint (time unknown)";
        return true;
    }
    if (v < BOILER_SET_MIN_X10) {
        *flow_x10 = 0;
        *src      = "schedule: boiler off";
    } else {
        *flow_x10 = v > BOILER_SET_MAX_X10 ? BOILER_SET_MAX_X10 : v;
        *src      = "schedule boiler setpoint";
    }
    return true;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void step(float dt_s)
{
    settings_t cfg;
    settings_get(&cfg);

    uint8_t  wd;
    uint16_t minute;
    bool     time_valid = clock_now(&wd, &minute);

    int16_t room;
    bool    sensor_ok = sensors_get_control((sensor_sel_e)cfg.control_sensor, &room);

    regulator_status_t prev;
    tal_mutex_lock(s_lock);
    prev = s_st;
    tal_mutex_unlock(s_lock);

    const char *src;
    int16_t     target;
    int16_t     set_x10;
    bool        demand = prev.demand;
    float       flow   = HEAT_FLOW_MIN_X10 / 10.0f;

    if (flow_setpoint(&cfg, time_valid, &set_x10, &src)) {
        /* The room is deliberately not considered (user's choice), except
         * frost protection when the boiler would otherwise stay off. */
        target = set_x10;
        if (set_x10 > 0) {
            demand = true;
            flow   = set_x10 / 10.0f;
        } else if (sensor_ok && room < HEAT_FROST_X10) {
            demand = true;
            flow   = HEAT_FLOW_FALLBACK_X10 / 10.0f;
            src    = "frost protection";
        } else {
            demand = false;
        }
    } else if (!sensor_ok) {
        target = room_target(&cfg, &src);
        demand = cfg.sw;
        flow   = HEAT_FLOW_FALLBACK_X10 / 10.0f;
        src    = cfg.sw ? "fallback flow (no sensor)" : "off (no sensor)";
    } else {
        target    = room_target(&cfg, &src);
        float err = (target - room) / 10.0f;
        if (err >= DEMAND_ON) {
            demand = true;
        } else if (err <= DEMAND_OFF) {
            demand = false;
        }

        float fmin = HEAT_FLOW_MIN_X10 / 10.0f, fmax = HEAT_FLOW_MAX_X10 / 10.0f;
        float raw  = target / 10.0f + FLOW_OFFSET + KP * err + s_integral;
        /* conditional integration: no windup while saturated or idle */
        bool saturated = (raw >= fmax && err > 0) || (raw <= fmin && err < 0);
        if (demand && !saturated) {
            s_integral = clampf(s_integral + KP * err * dt_s / TI_S, I_MIN, I_MAX);
        }
        flow = clampf(target / 10.0f + FLOW_OFFSET + KP * err + s_integral, fmin, fmax);
    }

    int16_t flow_x10 = (int16_t)(flow * 10.0f);
    boiler_set_demand(demand, flow_x10);
    {
        uint8_t rs = boiler_room_sensor();
        int16_t rv = 0;
        bool    ok = rs && sensors_get((uint8_t)(rs - 1), &rv);
        boiler_set_room(ok, rv);
    }

    boiler_status_t b;
    boiler_get_status(&b);
    energy_tick(dt_s, &b);

    uint32_t faults = 0;
    if (!sensor_ok) {
        faults |= FAULT_SENSOR;
    }
    if (!b.comm_ok || b.fault) {
        faults |= FAULT_HEATING;
    }

    work_state_e ws = b.flame ? WORK_HEATING : (demand ? WORK_WARM : WORK_IDLE);

    regulator_status_t now = {
        .time_valid   = time_valid,
        .sensor_ok    = sensor_ok,
        .room_x10     = sensor_ok ? room : prev.room_x10,
        .target_x10   = target,
        .target_src   = src,
        .demand       = demand,
        .flow_set_x10 = flow_x10,
        .integral_x10 = (int16_t)(s_integral * 10.0f),
        .faults       = faults,
        .work_state   = ws,
        .flow_valid   = b.flow_valid,
        .flow_x10     = b.flow_x10,
    };

    tal_mutex_lock(s_lock);
    s_st = now;
    tal_mutex_unlock(s_lock);

    uint32_t changed = 0;
    if (now.room_x10 != prev.room_x10) {
        changed |= REG_F_ROOM;
    }
    if (now.work_state != prev.work_state) {
        changed |= REG_F_STATE;
    }
    if (now.flow_valid && (!prev.flow_valid || (now.flow_x10 + 5) / 10 != (prev.flow_x10 + 5) / 10)) {
        changed |= REG_F_FLOW;
    }
    if (now.faults != prev.faults) {
        changed |= REG_F_FAULTS;
        PR_WARN("regulator: faults 0x%x (sensor=%d boiler_comm=%d boiler_fault=%d oem=%u)", faults, sensor_ok,
                b.comm_ok, b.fault, b.oem_fault);
    }
    if (now.demand != prev.demand) {
        PR_NOTICE("regulator: demand=%d target=%d (%s) room=%d flow_set=%d", demand, target, src, now.room_x10,
                  flow_x10);
    }
    if (changed && s_on_change) {
        s_on_change(changed);
    }
}

static void regulator_task(void *arg)
{
    int64_t last = ot_port_time_us();
    for (;;) {
        tal_system_sleep(PERIOD_MS);
        int64_t now = ot_port_time_us();
        step((now - last) / 1e6f);
        last = now;
        supervisor_beat(SUP_REGULATOR);
    }
}

void regulator_init(regulator_change_cb_t on_change)
{
    tal_mutex_create_init(&s_lock);
    s_on_change = on_change;
    s_st.room_x10   = INT16_MIN; /* forces the first report */
    s_st.work_state = WORK_IDLE;

    if (ot_port_task_create(regulator_task, "regulator", 4096, PRIO, CORE, NULL) != OT_PORT_OK) {
        PR_ERR("regulator: task create failed");
    }
}

void regulator_get_status(regulator_status_t *out)
{
    tal_mutex_lock(s_lock);
    *out = s_st;
    tal_mutex_unlock(s_lock);
}
