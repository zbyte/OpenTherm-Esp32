/**
 * @file boiler.h
 * @brief OpenTherm master: polls the boiler once a second on the protocol core.
 */

#ifndef BOILER_H_
#define BOILER_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     comm_ok;       /* false after several missed status exchanges */
    uint32_t comm_errors;   /* total failed exchanges since boot */
    bool     fault;         /* slave status: fault indication */
    bool     ch_active;     /* slave status: CH mode */
    bool     dhw_active;    /* slave status: DHW mode (3-way valve to the tank) */
    bool     flame;         /* slave status: flame (electric: elements on) */
    bool     flow_valid;
    int16_t  flow_x10;      /* ID25, 0.1 degC */
    bool     mod_valid;
    int16_t  rel_mod_x10;   /* ID17, 0.1 % */
    bool     pressure_valid;
    int16_t  pressure_x100; /* ID18, 0.01 bar */
    bool     dhw_temp_valid;
    int16_t  dhw_temp_x10;  /* ID26, 0.1 degC: hot water tank */
    uint8_t  asf_flags;     /* ID5 high byte */
    uint8_t  oem_fault;     /* ID5 low byte */
    bool     tset_ok;       /* boiler acknowledged the last ID1 write */
    bool     ch_enable_cmd; /* last commanded */
    bool     dhw_enable_cmd;
    uint8_t  room_sensor;   /* sensor slot 1-4 sent as ID24 room temperature, 0 = off */
    bool     room_valid;    /* that sensor has a fresh value */
    int16_t  room_x10;
    bool     room_ok;       /* boiler acknowledged the last ID24 write */
    uint32_t power_limit_w; /* sent as ID14 max modulation (BAXI Ampera: menu 5.0), 0 = not sent */
    bool     power_ok;      /* boiler acknowledged it */
    int16_t  tset_x10_cmd;
} boiler_status_t;

/* OpenTherm line quality since boot, every exchange (task, CLI and cloud). */
typedef struct {
    uint32_t total;    /* exchanges */
    uint32_t timeouts; /* no answer (also: the boiler ignores some ids) */
    uint32_t framing;  /* answer started but the Manchester timing broke */
    uint32_t parity;   /* answer with a parity error */
    uint32_t echo;     /* answer for another id, with a master message type, or
                        * a WRITE_ACK echoing another value than was written */
    uint32_t bad_seq;  /* bumped on every framing, parity or echo error */
    char     last_bad[48]; /* "parity > 10013d00 < d0013d01" */
} boiler_bus_stats_t;

void boiler_get_bus_stats(boiler_bus_stats_t *out);

void boiler_init(void);

/* Demand from the regulator; sent on the next cycle. */
void boiler_set_demand(bool ch_enable, int16_t tset_x10);

/* Power limit for ID14, W (0 = do not send); a percentage of BOILER_POWER_W. */
void boiler_set_power_limit(uint32_t watts);

/* DHW enable bit in ID0: the boiler then also heats its hot water tank. */
void boiler_set_dhw(bool on);

/* Room temperature for ID24: which sensor slot (1-4, 0 = do not send) and
 * its current value (fed by the regulator every tick). */
void    boiler_set_room_sensor(uint8_t slot);
uint8_t boiler_room_sensor(void);
void    boiler_set_room(bool valid, int16_t x10);

void boiler_get_status(boiler_status_t *out);

/* Log every exchange (CLI: ot log on|off). */
void boiler_set_trace(bool on);

/* One-off raw exchange for debugging (CLI: ot raw). Returns ot_port_err_e. */
int boiler_raw(uint8_t type, uint8_t id, uint16_t value, uint32_t *response);

#endif /* BOILER_H_ */
