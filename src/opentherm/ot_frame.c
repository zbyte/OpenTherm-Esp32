/**
 * @file ot_frame.c
 * @brief OpenTherm 2.2 frame encoding.
 */

#include "ot_frame.h"

static bool odd_bits(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1u;
}

uint32_t ot_frame_build(ot_msg_type_e type, uint8_t id, uint16_t value)
{
    uint32_t f = ((uint32_t)(type & 0x7) << 28) | ((uint32_t)id << 16) | value;
    if (odd_bits(f)) {
        f |= 1u << 31;
    }
    return f;
}

bool ot_frame_parity_ok(uint32_t frame)
{
    return !odd_bits(frame);
}

const char *ot_msg_type_str(ot_msg_type_e type)
{
    static const char *const names[] = {"READ_DATA", "WRITE_DATA", "INVALID_DATA", "RESERVED",
                                        "READ_ACK",  "WRITE_ACK",  "DATA_INVALID", "UNKNOWN_DATAID"};
    return names[type & 0x7];
}
