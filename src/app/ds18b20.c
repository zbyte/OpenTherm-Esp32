/**
 * @file ds18b20.c
 * @brief DS18B20 room sensors, one per GPIO (no ROM search), into sensor slots.
 *
 * Non-blocking cycle every 2 s: read the scratchpad of the conversion started
 * in the previous cycle (12-bit needs 750 ms), then start the next one. Runs
 * on the network core; 1-Wire slots never touch the OpenTherm core.
 * Every 30 s the ROM code and the power mode are read too; a parasite-powered
 * sensor (two wires) gets a strong pull-up for the conversion.
 */

#include "ds18b20.h"

#include <stdlib.h>
#include <string.h>

#include "tal_api.h"
#include "tuya_kconfig.h"

#include "ot_port.h"
#include "sensors.h"

#define PERIOD_MS  2000
#define DIAG_EVERY 15 /* cycles */
#define CORE       0
#define PRIO       5

#define CMD_READ_ROM   0x33
#define CMD_SKIP_ROM   0xCC
#define CMD_CONVERT    0x44
#define CMD_READ_SCRPD 0xBE
#define CMD_READ_POWER 0xB4

/* Clone sensors short of power return well-formed scratchpads with wild
 * values (-41 degC at room temperature), up to a third of the readings.
 * The first value, and any reading far from the published one, is held
 * back until the next ones confirm it; the rest goes through a median. */
#define MEDIAN_N    5  /* 10 s window */
#define JUMP_X10    30 /* a room does not change 3 degC in 2 s */
#define CONFIRM_N   3  /* consecutive readings that make a jump real */
#define CONFIRM_X10 10 /* ...agreeing within 1 degC */

#define FAMILY_DS18B20 0x28
#define POWER_ON_RAW   0x0550 /* 85.0 degC: reset value, not a reading */

typedef enum {
    POWER_UNKNOWN,
    POWER_VDD,
    POWER_PARASITE,
} power_e;

typedef struct {
    int      gpio;
    uint8_t  slot;
    bool     converting;
    uint32_t ok;
    uint32_t no_presence;
    uint32_t stuck_low;
    uint32_t bad_crc;
    uint32_t bad_data; /* CRC fine, but not a DS18B20 scratchpad / 85 degC */
    int16_t  last_x10; /* published (median) */
    int16_t  hist[MEDIAN_N];
    uint8_t  hist_n;
    uint8_t  hist_pos;
    bool     locked;   /* a confirmed value has been published */
    int16_t  cand_x10; /* held-back first value or jump */
    uint8_t  cand_n;
    uint32_t spikes;
    int16_t  last_spike_x10;
    uint8_t  sp[9];     /* last scratchpad read */
    uint8_t  bad_sp[9]; /* last one rejected */
    uint8_t  rom[8];
    bool     rom_ok;
    uint32_t rom_errors;
    power_e  power;
    uint32_t cycle;
} ds_t;

static ds_t s_ds[] = {
    {.gpio = DS18B20_1_GPIO, .slot = 0},
    {.gpio = DS18B20_2_GPIO, .slot = 1},
};

static volatile ds18b20_pull_e s_pull = DS18B20_PULL_AUTO;

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

/* Reset + SKIP ROM; counts the reason when nobody answers. */
static bool ow_select(ds_t *d)
{
    switch (ow_port_reset(d->gpio)) {
    case OW_PRESENT:
        ow_port_write_byte(d->gpio, CMD_SKIP_ROM);
        return true;
    case OW_STUCK_LOW:
        d->stuck_low++;
        return false;
    default:
        d->no_presence++;
        return false;
    }
}

static int16_t median(const ds_t *d)
{
    int16_t v[MEDIAN_N];
    memcpy(v, d->hist, sizeof(v));
    for (int i = 1; i < d->hist_n; i++) { /* insertion sort, n <= 5 */
        int16_t t = v[i];
        int     j = i;
        while (j > 0 && v[j - 1] > t) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = t;
    }
    return v[d->hist_n / 2];
}

static void publish(ds_t *d, int16_t x10)
{
    if (!d->locked || abs(x10 - d->last_x10) > JUMP_X10) {
        if (d->cand_n > 0 && abs(x10 - d->cand_x10) <= CONFIRM_X10) {
            d->cand_n++;
        } else {
            d->cand_x10 = x10;
            d->cand_n   = 1;
        }
        if (d->cand_n < CONFIRM_N) {
            if (d->locked) {
                d->spikes++;
                d->last_spike_x10 = x10;
                memcpy(d->bad_sp, d->sp, sizeof(d->sp));
            }
            return;
        }
        d->hist_n   = 0; /* confirmed: start the median over from here */
        d->hist_pos = 0;
        d->locked   = true;
    }
    d->cand_n = 0;

    d->hist[d->hist_pos] = x10;
    d->hist_pos          = (uint8_t)((d->hist_pos + 1) % MEDIAN_N);
    if (d->hist_n < MEDIAN_N) {
        d->hist_n++;
    }
    d->last_x10 = median(d);
    sensors_set(d->slot, d->last_x10);
}

static void read_temp(ds_t *d)
{
    if (!ow_select(d)) {
        return;
    }
    ow_port_write_byte(d->gpio, CMD_READ_SCRPD);
    for (int i = 0; i < 9; i++) {
        d->sp[i] = ow_port_read_byte(d->gpio);
    }
    if (crc8(d->sp, 8) != d->sp[8]) {
        d->bad_crc++;
        memcpy(d->bad_sp, d->sp, sizeof(d->sp));
        return;
    }
    /* All-zero data has a valid CRC too; the config register (byte 4) of a
     * real DS18B20 is always 0b0RR11111. */
    int16_t raw = (int16_t)((d->sp[1] << 8) | d->sp[0]);
    if ((d->sp[4] & 0x9F) != 0x1F || (uint16_t)raw == POWER_ON_RAW) {
        d->bad_data++;
        memcpy(d->bad_sp, d->sp, sizeof(d->sp));
        return;
    }
    d->ok++;
    publish(d, (int16_t)((raw * 10) / 16));
}

static void read_diag(ds_t *d)
{
    if (ow_port_reset(d->gpio) != OW_PRESENT) {
        return;
    }
    uint8_t rom[8];
    ow_port_write_byte(d->gpio, CMD_READ_ROM);
    for (int i = 0; i < 8; i++) {
        rom[i] = ow_port_read_byte(d->gpio);
    }
    if (crc8(rom, 7) != rom[7] || rom[0] != FAMILY_DS18B20) {
        d->rom_errors++;
        return; /* a garbled read must not decide the power mode either */
    }
    memcpy(d->rom, rom, sizeof(rom));
    d->rom_ok = true;

    if (ow_select(d)) {
        ow_port_write_byte(d->gpio, CMD_READ_POWER);
        d->power = ow_port_read_bit(d->gpio) ? POWER_VDD : POWER_PARASITE;
    }
}

static bool start_convert(ds_t *d)
{
    if (!ow_select(d)) {
        return false;
    }
    ow_port_write_byte(d->gpio, CMD_CONVERT);
    if (s_pull == DS18B20_PULL_ON || (s_pull == DS18B20_PULL_AUTO && d->power == POWER_PARASITE)) {
        ow_port_strong_pullup(d->gpio); /* 4.7k cannot feed the conversion */
    }
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
                read_temp(d);
            }
            if (d->cycle++ % DIAG_EVERY == 0) {
                read_diag(d);
            }
            d->converting = start_convert(d);
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

void ds18b20_set_pull(ds18b20_pull_e mode)
{
    s_pull = mode;
    for (size_t i = 0; i < sizeof(s_ds) / sizeof(s_ds[0]); i++) {
        ds_t *d        = &s_ds[i];
        d->ok          = 0;
        d->no_presence = 0;
        d->stuck_low   = 0;
        d->bad_crc     = 0;
        d->bad_data    = 0;
        d->rom_errors  = 0;
        d->spikes      = 0;
    }
}

void ds18b20_print(void)
{
    static const char *const power[] = {"?", "vdd", "parasite"};
    static const char *const pull[]  = {"auto", "on", "off"};

    for (size_t i = 0; i < sizeof(s_ds) / sizeof(s_ds[0]); i++) {
        const ds_t     *d = &s_ds[i];
        const uint8_t *s = d->sp;
        const uint8_t *b = d->bad_sp;
        const uint8_t *r = d->rom;
        PR_NOTICE("ds18b20 #%u: GPIO%d slot %u ok=%u nopres=%u stuck=%u crc=%u bad=%u spikes=%u (last %d) median=%d "
                  "power=%s pull=%s",
                  (unsigned)(i + 1), d->gpio, d->slot + 1, d->ok, d->no_presence, d->stuck_low, d->bad_crc,
                  d->bad_data, d->spikes, d->last_spike_x10, d->last_x10, power[d->power], pull[s_pull]);
        PR_NOTICE("  sp=%02x %02x %02x %02x %02x %02x %02x %02x %02x  last bad=%02x %02x %02x %02x %02x %02x %02x %02x %02x",
                  s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                  b[8]);
        PR_NOTICE("  rom=%02x%02x%02x%02x%02x%02x%02x%02x %s, rom read errors=%u", r[0], r[1], r[2], r[3], r[4], r[5],
                  r[6], r[7], d->rom_ok ? "ok" : "none", d->rom_errors);
    }
}
