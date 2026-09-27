/**
 * @file bmp180.c
 * @brief On-board BMP180 (GY-68): temperature -> sensor slot 3, pressure.
 *
 * Compensation per the Bosch BMP180 datasheet (integer algorithm, oss = 1).
 */

#include "bmp180.h"

#include "tal_api.h"
#include "tuya_kconfig.h"

#include "ot_port.h"
#include "sensors.h"

#define ADDR       0x77
#define REG_CALIB  0xAA
#define REG_ID     0xD0
#define REG_CTRL   0xF4
#define REG_DATA   0xF6
#define CHIP_ID    0x55
#define OSS        1
#define PERIOD_MS  10000
#define CORE       0
#define PRIO       5

typedef struct {
    int16_t  ac1, ac2, ac3;
    uint16_t ac4, ac5, ac6;
    int16_t  b1, b2, mb, mc, md;
} calib_t;

static calib_t          s_cal;
static bool             s_present;
static volatile bool    s_have;
static volatile int32_t s_pa;
static volatile int16_t s_t_x10;
static uint32_t         s_ok, s_err;

static bool read_calib(void)
{
    uint8_t b[22];
    if (i2c_port_read_reg(ADDR, REG_CALIB, b, sizeof(b)) != OT_PORT_OK) {
        return false;
    }
#define U16(i) ((uint16_t)((b[i] << 8) | b[(i) + 1]))
    s_cal = (calib_t){
        .ac1 = (int16_t)U16(0), .ac2 = (int16_t)U16(2), .ac3 = (int16_t)U16(4), .ac4 = U16(6),
        .ac5 = U16(8),          .ac6 = U16(10),         .b1 = (int16_t)U16(12), .b2 = (int16_t)U16(14),
        .mb = (int16_t)U16(16), .mc = (int16_t)U16(18), .md = (int16_t)U16(20),
    };
#undef U16
    /* all-0x00 or all-0xFF calibration means a dead bus, not a sensor */
    return s_cal.ac1 != 0 && s_cal.ac1 != -1 && s_cal.md != 0;
}

static bool measure(int16_t *t_x10, int32_t *pa)
{
    uint8_t cmd[2], d[3];

    cmd[0] = REG_CTRL;
    cmd[1] = 0x2E;
    if (i2c_port_write(ADDR, cmd, 2) != OT_PORT_OK) {
        return false;
    }
    tal_system_sleep(6);
    if (i2c_port_read_reg(ADDR, REG_DATA, d, 2) != OT_PORT_OK) {
        return false;
    }
    int32_t ut = (d[0] << 8) | d[1];

    cmd[1] = 0x34 + (OSS << 6);
    if (i2c_port_write(ADDR, cmd, 2) != OT_PORT_OK) {
        return false;
    }
    tal_system_sleep(9);
    if (i2c_port_read_reg(ADDR, REG_DATA, d, 3) != OT_PORT_OK) {
        return false;
    }
    int32_t up = (((int32_t)d[0] << 16) | ((int32_t)d[1] << 8) | d[2]) >> (8 - OSS);

    int32_t x1 = ((ut - s_cal.ac6) * s_cal.ac5) >> 15;
    int32_t x2 = ((int32_t)s_cal.mc << 11) / (x1 + s_cal.md);
    int32_t b5 = x1 + x2;
    *t_x10     = (int16_t)((b5 + 8) >> 4);

    int32_t  b6 = b5 - 4000;
    x1          = (s_cal.b2 * ((b6 * b6) >> 12)) >> 11;
    x2          = (s_cal.ac2 * b6) >> 11;
    int32_t x3  = x1 + x2;
    int32_t b3  = ((((int32_t)s_cal.ac1 * 4 + x3) << OSS) + 2) >> 2;
    x1          = (s_cal.ac3 * b6) >> 13;
    x2          = (s_cal.b1 * ((b6 * b6) >> 12)) >> 16;
    x3          = ((x1 + x2) + 2) >> 2;
    uint32_t b4 = ((uint32_t)s_cal.ac4 * (uint32_t)(x3 + 32768)) >> 15;
    uint32_t b7 = ((uint32_t)up - b3) * (uint32_t)(50000 >> OSS);
    int32_t  p  = b7 < 0x80000000u ? (int32_t)((b7 * 2) / b4) : (int32_t)((b7 / b4) * 2);
    x1          = (p >> 8) * (p >> 8);
    x1          = (x1 * 3038) >> 16;
    x2          = (-7357 * p) >> 16;
    *pa         = p + ((x1 + x2 + 3791) >> 4);
    return true;
}

static void bmp_task(void *arg)
{
    for (;;) {
        if (!s_present) {
            uint8_t id = 0;
            s_present  = i2c_port_read_reg(ADDR, REG_ID, &id, 1) == OT_PORT_OK && id == CHIP_ID && read_calib();
            if (s_present) {
                PR_NOTICE("bmp180: found at 0x%02x", ADDR);
            }
        }
        if (s_present) {
            int16_t t;
            int32_t pa;
            /* sanity window rejects bus garbage */
            if (measure(&t, &pa) && t > -400 && t < 850 && pa > 30000 && pa < 110000) {
                s_t_x10 = t;
                s_pa    = pa;
                s_have  = true;
                s_ok++;
                sensors_set(BMP180_SLOT, t);
            } else {
                s_err++;
                s_present = false; /* re-probe next time */
            }
        }
        tal_system_sleep(PERIOD_MS);
    }
}

void bmp180_init(void)
{
    if (i2c_port_init(I2C_SDA_GPIO, I2C_SCL_GPIO) != OT_PORT_OK ||
        ot_port_task_create(bmp_task, "bmp180", 3072, PRIO, CORE, NULL) != OT_PORT_OK) {
        PR_ERR("bmp180: init failed");
    }
}

bool bmp180_pressure(int32_t *pa)
{
    if (!s_have) {
        return false;
    }
    *pa = s_pa;
    return true;
}

void bmp180_print(void)
{
    PR_NOTICE("bmp180: SDA=GPIO%d SCL=GPIO%d present=%d ok=%u err=%u t=%d p=%dPa", I2C_SDA_GPIO, I2C_SCL_GPIO,
              s_present, s_ok, s_err, s_t_x10, (int)s_pa);
}
