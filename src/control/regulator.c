/**
 * @file regulator.c
 * @brief Room temperature control: target selection, PI -> flow setpoint.
 *
 * Target: switch off -> frost temperature; mode auto -> weekly program (or
 * temp_set while the time is unknown); manual/home -> temp_set; leave ->
 * away temperature. The PI output is the boiler flow setpoint; the boiler
 * stages its elements to hold it.
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
#include "ot_port.h"
#include "sensors.h"
#include "settings.h"

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

static int16_t select_target(const settings_t *cfg, bool time_valid, const char **src)
{
    if (!cfg->sw) {
        *src = "frost";
        return HEAT_FROST_X10;
    }
    switch (cfg->mode) {
    case HEAT_MODE_AUTO: {
        uint8_t  wd;
        uint16_t minute;
        int16_t  t;
        if (time_valid && clock_now(&wd, &minute) && schedule_target(cfg->schedule, wd, minute, &t)) {
            *src = "schedule";
            return t;
        }
        *src = "temp_set (no time)";
        return cfg->temp_set_x10;
    }
    case HEAT_MODE_LEAVE:
        *src = "away";
        return HEAT_AWAY_X10;
    default:
        *src = "temp_set";
        return cfg->temp_set_x10;
    }
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

    const char *src;
    int16_t     target = select_target(&cfg, time_valid, &src);

    regulator_status_t prev;
    tal_mutex_lock(s_lock);
    prev = s_st;
    tal_mutex_unlock(s_lock);

    bool  demand = prev.demand;
    float flow   = HEAT_FLOW_MIN_X10 / 10.0f;

    if (!sensor_ok) {
        demand = cfg.sw;
        flow   = HEAT_FLOW_FALLBACK_X10 / 10.0f;
        src    = cfg.sw ? "fallback flow (no sensor)" : "off (no sensor)";
    } else {
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

    boiler_status_t b;
    boiler_get_status(&b);

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
