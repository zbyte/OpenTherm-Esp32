/**
 * @file i2c_port.c
 * @brief Minimal I2C master for on-board sensors (ESP-IDF i2c_master driver).
 */

#include "ot_port.h"

#include "driver/i2c_master.h"

#define I2C_TIMEOUT_MS 50

static i2c_master_bus_handle_t s_bus;

int i2c_port_init(int sda, int scl)
{
    if (s_bus) {
        return OT_PORT_OK;
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port                     = I2C_NUM_1, /* TKL owns I2C_NUM_0 if ever used */
        .sda_io_num                   = sda,
        .scl_io_num                   = scl,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true, /* GY-68 has its own pull-ups */
    };
    return i2c_new_master_bus(&cfg, &s_bus) == ESP_OK ? OT_PORT_OK : OT_PORT_ERR;
}

bool i2c_port_probe(uint8_t addr)
{
    return s_bus && i2c_master_probe(s_bus, addr, I2C_TIMEOUT_MS) == ESP_OK;
}

static i2c_master_dev_handle_t dev_for(uint8_t addr)
{
    /* one sensor on this bus: cache the handle of the last address */
    static i2c_master_dev_handle_t dev;
    static uint8_t                 dev_addr;
    if (dev && dev_addr == addr) {
        return dev;
    }
    if (dev) {
        i2c_master_bus_rm_device(dev);
        dev = NULL;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(s_bus, &cfg, &dev) != ESP_OK) {
        dev = NULL;
    }
    dev_addr = addr;
    return dev;
}

int i2c_port_write(uint8_t addr, const uint8_t *data, size_t len)
{
    i2c_master_dev_handle_t dev = s_bus ? dev_for(addr) : NULL;
    return dev && i2c_master_transmit(dev, data, len, I2C_TIMEOUT_MS) == ESP_OK ? OT_PORT_OK : OT_PORT_ERR;
}

int i2c_port_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    i2c_master_dev_handle_t dev = s_bus ? dev_for(addr) : NULL;
    return dev && i2c_master_transmit_receive(dev, &reg, 1, data, len, I2C_TIMEOUT_MS) == ESP_OK ? OT_PORT_OK
                                                                                                 : OT_PORT_ERR;
}
