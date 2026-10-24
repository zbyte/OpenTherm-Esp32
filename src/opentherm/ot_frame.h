/**
 * @file ot_frame.h
 * @brief OpenTherm 2.2 frame encoding.
 *
 * Frame: [31] parity (even over all 32 bits), [30:28] message type,
 * [27:24] spare, [23:16] data id, [15:0] data value.
 */

#ifndef OT_FRAME_H_
#define OT_FRAME_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    /* master to slave */
    OT_READ_DATA    = 0,
    OT_WRITE_DATA   = 1,
    OT_INVALID_DATA = 2,
    /* slave to master */
    OT_READ_ACK      = 4,
    OT_WRITE_ACK     = 5,
    OT_DATA_INVALID  = 6,
    OT_UNKNOWN_DATAID = 7,
} ot_msg_type_e;

/* Data ids used by this firmware. */
enum {
    OT_ID_STATUS        = 0,  /* HB master flags, LB slave flags */
    OT_ID_TSET          = 1,  /* control setpoint, f8.8 degC */
    OT_ID_MASTER_CONFIG = 2,  /* HB master flags, LB master member id */
    OT_ID_SLAVE_CONFIG  = 3,  /* HB slave flags, LB member id */
    OT_ID_ASF_FLAGS     = 5,  /* HB application-specific fault flags, LB OEM fault code */
    OT_ID_MAX_REL_MOD   = 14, /* max relative modulation, f8.8 % */
    OT_ID_TROOM         = 24, /* room temperature, f8.8 degC (master writes) */
    OT_ID_REL_MOD       = 17, /* relative modulation level, f8.8 % */
    OT_ID_CH_PRESSURE   = 18, /* water pressure in the CH circuit, f8.8 bar */
    OT_ID_TBOILER       = 25, /* flow temperature, f8.8 degC */
    OT_ID_TDHW          = 26, /* DHW (tank) temperature, f8.8 degC */
    OT_ID_TRET          = 28, /* return temperature, f8.8 degC */
    OT_ID_TSET_BOUNDS   = 49, /* HB upper, LB lower bound of max CH setpoint, degC */
    OT_ID_MAX_TSET      = 57, /* max CH setpoint, f8.8 degC */
    OT_ID_OT_VERSION_S  = 125,
    OT_ID_OEM_DIAG      = 115,
};

/* ID0 master status (high byte) */
#define OT_MASTER_CH_ENABLE  (1u << 0)
#define OT_MASTER_DHW_ENABLE (1u << 1)

/* ID0 slave status (low byte) */
#define OT_SLAVE_FAULT     (1u << 0)
#define OT_SLAVE_CH_ACTIVE (1u << 1)
#define OT_SLAVE_DHW_ACTIVE (1u << 2)
#define OT_SLAVE_FLAME     (1u << 3) /* electric boiler: heating elements on */
#define OT_SLAVE_DIAG      (1u << 6)

uint32_t ot_frame_build(ot_msg_type_e type, uint8_t id, uint16_t value);
bool     ot_frame_parity_ok(uint32_t frame);

static inline ot_msg_type_e ot_frame_type(uint32_t f)
{
    return (ot_msg_type_e)((f >> 28) & 0x7);
}
static inline uint8_t ot_frame_id(uint32_t f)
{
    return (uint8_t)(f >> 16);
}
static inline uint16_t ot_frame_value(uint32_t f)
{
    return (uint16_t)f;
}

/* f8.8 <-> tenths of a degree (or percent) */
static inline int16_t ot_f88_to_x10(uint16_t v)
{
    return (int16_t)(((int32_t)(int16_t)v * 10) / 256);
}
static inline uint16_t ot_x10_to_f88(int16_t x10)
{
    return (uint16_t)(int16_t)(((int32_t)x10 * 256) / 10);
}

const char *ot_msg_type_str(ot_msg_type_e type);

#endif /* OT_FRAME_H_ */
