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
    bool     flame;         /* slave status: flame (electric: elements on) */
    bool     flow_valid;
    int16_t  flow_x10;      /* ID25, 0.1 degC */
    bool     mod_valid;
    int16_t  rel_mod_x10;   /* ID17, 0.1 % */
    uint8_t  asf_flags;     /* ID5 high byte */
    uint8_t  oem_fault;     /* ID5 low byte */
    bool     ch_enable_cmd; /* last commanded */
    int16_t  tset_x10_cmd;
} boiler_status_t;

void boiler_init(void);

/* Demand from the regulator; sent on the next cycle. */
void boiler_set_demand(bool ch_enable, int16_t tset_x10);

void boiler_get_status(boiler_status_t *out);

/* Log every exchange (CLI: ot log on|off). */
void boiler_set_trace(bool on);

/* One-off raw exchange for debugging (CLI: ot raw). Returns ot_port_err_e. */
int boiler_raw(uint8_t type, uint8_t id, uint16_t value, uint32_t *response);

#endif /* BOILER_H_ */
