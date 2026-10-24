/**
 * @file regulator.h
 * @brief Room temperature control: target selection, PI -> flow setpoint.
 */

#ifndef REGULATOR_H_
#define REGULATOR_H_

#include <stdbool.h>
#include <stdint.h>

/* fault DP bitmap labels, in product-model order */
#define FAULT_SENSOR  (1u << 0) /* sensor_fault */
#define FAULT_MOTOR   (1u << 1) /* motor_fault (unused) */
#define FAULT_HEATING (1u << 2) /* heating_fault: boiler fault or no OpenTherm link */

/* work_state DP enum, in product-model order */
typedef enum {
    WORK_HEATING = 0, /* elements on */
    WORK_WARM    = 1, /* heat demanded, boiler holding temperature */
    WORK_IDLE    = 2,
} work_state_e;

typedef struct {
    bool         time_valid;
    bool         sensor_ok;
    int16_t      room_x10;
    int16_t      target_x10;
    const char  *target_src; /* "schedule", "temp_set", ... */
    bool         demand;
    int16_t      flow_set_x10;
    int16_t      integral_x10;
    uint32_t     faults;
    work_state_e work_state;
    bool         flow_valid;
    int16_t      flow_x10; /* measured boiler flow temperature */
    bool         stalled;  /* heat demanded, flow well below the setpoint, boiler idle for long */
} regulator_status_t;

/* fields for the change callback */
#define REG_F_ROOM   (1u << 0)
#define REG_F_STATE  (1u << 1)
#define REG_F_FAULTS (1u << 2)
#define REG_F_FLOW   (1u << 3) /* boiler flow temperature, whole degrees */

typedef void (*regulator_change_cb_t)(uint32_t fields);

void regulator_init(regulator_change_cb_t on_change);
void regulator_get_status(regulator_status_t *out);

#endif /* REGULATOR_H_ */
