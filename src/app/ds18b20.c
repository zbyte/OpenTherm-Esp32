/**
 * @file ds18b20.c
 * @brief DS18B20 room sensors, one per GPIO (no ROM search), into sensor slots.
 *
 * Non-blocking cycle every 2 s: read the scratchpad of the conversion started
 * in the previous cycle (12-bit needs 750 ms), then start the next one. Runs
 * on the network core; 1-Wire slots never touch the OpenTherm core.
 */

#include "ds18b20.h"

#include "tal_api.h"
#include "tuya_kconfig.h"

#include "ot_port.h"
#include "sensors.h"

#define PERIOD_MS 2000
#define CORE      0
#define PRIO      5

#define CMD_SKIP_ROM   0xCC
#define CMD_CONVERT    0x44
#define CMD_READ_SCRPD 0xBE

#define POWER_ON_RAW 0x0550 /* 85.0 degC: reset value, not a reading */

typedef struct {
    int      gpio;
    uint8_t  slot;
    bool     converting;
    uint32_t ok;
    uint32_t errors;
    int16_t  last_x10;
} ds_t;

static ds_t s_ds[] = {
    {.gpio = DS18B20_1_GPIO, .slot = 0},
    {.gpio = DS18B20_2_GPIO, .slot = 1},
};

static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t crc = 0;
    while (n--) {
        uint8_t b = *d++;
        for (int i = 0; i < 8; i++) {
            uint8_t mix = (crc ^ b) & 1u;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            b >>= 1;
        }
    }
    return crc;
}

static bool read_temp(int gpio, int16_t *x10)
{
    uint8_t sp[9];

    if (!ow_port_reset(gpio)) {
        return false;
    }
    ow_port_write_byte(gpio, CMD_SKIP_ROM);
    ow_port_write_byte(gpio, CMD_READ_SCRPD);
    for (int i = 0; i < 9; i++) {
        sp[i] = ow_port_read_byte(gpio);
    }
    /* All-zero data has a valid CRC too; the config register (byte 4) of a
     * real DS18B20 is always 0b0RR11111. */
    if (crc8(sp, 8) != sp[8] || (sp[4] & 0x9F) != 0x1F) {
        return false;
    }
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    if ((uint16_t)raw == POWER_ON_RAW) {
        return false;
    }
    *x10 = (int16_t)((raw * 10) / 16);
    return true;
}

static bool start_convert(int gpio)
{
    if (!ow_port_reset(gpio)) {
        return false;
    }
    ow_port_write_byte(gpio, CMD_SKIP_ROM);
    ow_port_write_byte(gpio, CMD_CONVERT);
    return true;
}

static void ds_task(void *arg)
{
    for (;;) {
        for (size_t i = 0; i < sizeof(s_ds) / sizeof(s_ds[0]); i++) {
            ds_t *d = &s_ds[i];
            if (d->gpio < 0) {
                continue;
            }
            if (d->converting) {
                int16_t v;
                if (read_temp(d->gpio, &v)) {
                    d->ok++;
                    d->last_x10 = v;
                    sensors_set(d->slot, v);
                } else {
                    d->errors++;
                }
            }
            d->converting = start_convert(d->gpio);
            if (!d->converting) {
                d->errors++; /* no presence pulse: absent or wiring fault */
            }
        }
        tal_system_sleep(PERIOD_MS);
    }
}

void ds18b20_init(void)
{
    for (size_t i = 0; i < sizeof(s_ds) / sizeof(s_ds[0]); i++) {
        if (s_ds[i].gpio >= 0 && ow_port_init(s_ds[i].gpio) != OT_PORT_OK) {
            PR_ERR("ds18b20: GPIO%d init failed", s_ds[i].gpio);
            s_ds[i].gpio = -1;
        }
    }
    if (ot_port_task_create(ds_task, "ds18b20", 3072, PRIO, CORE, NULL) != OT_PORT_OK) {
        PR_ERR("ds18b20: task create failed");
    }
}

void ds18b20_print(void)
{
    for (size_t i = 0; i < sizeof(s_ds) / sizeof(s_ds[0]); i++) {
        const ds_t *d = &s_ds[i];
        PR_NOTICE("ds18b20 #%u: GPIO%d slot %u ok=%u err=%u last=%d", (unsigned)(i + 1), d->gpio, d->slot + 1, d->ok,
                  d->errors, d->last_x10);
    }
}
