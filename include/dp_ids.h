/**
 * @file dp_ids.h
 * @brief Data points of the "Opentherm esp32" product (category wk).
 *
 * From the product model read via Tuya OpenAPI on 2026-09-26
 * (/v2.0/cloud/thing/{id}/model). Value DPs are integers scaled as noted.
 */

#ifndef DP_IDS_H_
#define DP_IDS_H_

#define DP_ID_SWITCH       1  /* bool rw */
#define DP_ID_TEMP_SET     2  /* value rw, 5.0..35.0 degC, scale 1, step 0.5 */
#define DP_ID_TEMP_CURRENT 3  /* value ro, -20..80 degC, scale 0 */
#define DP_ID_MODE         4  /* enum rw: auto, manual, home, leave */
#define DP_ID_WORK_STATE   5  /* enum ro: heating, warm, idle */
#define DP_ID_WORK_POWER   10 /* value ro, 0..18000 W, scale 0 (estimated boiler power) */
#define DP_ID_FAULT        11 /* bitmap ro: sensor_fault, motor_fault, heating_fault */
#define DP_ID_UPPER_TEMP   15 /* value rw, 25.0..35.0 degC, scale 1 */
#define DP_ID_LOWER_TEMP   16 /* value rw, 5.0..15.0 degC, scale 1 */
#define DP_ID_WEEK_PROGRAM 28 /* raw rw, week_program_2, see schedule.h */

/* Custom DPs (added by the user on the platform) */
#define DP_ID_BOILER_SET 101 /* boiler_temp_set: value rw, 20..80 degC, scale 0 */
#define DP_ID_FLOW_TEMP  102 /* flow_temp: value ro, 0..100 degC, scale 0 */
#define DP_ID_TEMP_S1    103 /* temp_sensor1: DS18B20 on J2, value ro, -40.0..125.0 degC, scale 1 */
#define DP_ID_TEMP_S2    104 /* temp_sensor2: DS18B20 on J3, value ro, -40.0..125.0 degC, scale 1 */
#define DP_ID_TEMP_BOARD 105 /* temp_board: BMP180 on the board, value ro, -40.0..85.0 degC, scale 1 */
#define DP_ID_PRESSURE   106 /* pressure: BMP180, value ro, 300..1100 hPa, scale 0 */
#define DP_ID_ELE_TOTAL  107 /* ele_total: estimated energy, value ro, 0.01 kWh, scale 2 */

#endif /* DP_IDS_H_ */
