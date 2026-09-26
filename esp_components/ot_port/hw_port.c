/**
 * @file hw_port.c
 * @brief Board services on ESP-IDF: WS2812 LED, task watchdog, button,
 *        reset reason, OTA rollback confirmation.
 */

#include "ot_port.h"

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"

/* ------------------------------------------------------------- WS2812 */

#define LED_RMT_HZ 10000000 /* 0.1 us ticks */

static rmt_channel_handle_t s_led_chan;
static rmt_encoder_handle_t s_led_enc;
static bool                 s_led_grb = true;

int led_port_init(int gpio, bool grb)
{
    rmt_tx_channel_config_t ch = {
        .gpio_num          = gpio,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = LED_RMT_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    if (rmt_new_tx_channel(&ch, &s_led_chan) != ESP_OK) {
        return OT_PORT_ERR;
    }
    /* WS2812: '0' = 0.3 us high + 0.9 us low, '1' = 0.9 us high + 0.3 us low */
    rmt_bytes_encoder_config_t enc = {
        .bit0           = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1           = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    if (rmt_new_bytes_encoder(&enc, &s_led_enc) != ESP_OK || rmt_enable(s_led_chan) != ESP_OK) {
        return OT_PORT_ERR;
    }
    s_led_grb = grb;
    return OT_PORT_OK;
}

void led_port_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led_chan) {
        return;
    }
    uint8_t px[3] = {s_led_grb ? g : r, s_led_grb ? r : g, b};
    rmt_transmit_config_t tx = {.loop_count = 0};
    rmt_transmit(s_led_chan, s_led_enc, px, sizeof(px), &tx);
    rmt_tx_wait_all_done(s_led_chan, 10); /* the >50 us gap latches it */
}

/* ------------------------------------------------------------ watchdog */

int wdt_port_watch_self(uint32_t timeout_ms)
{
    /* ESP-IDF starts the task watchdog (a hardware timer-group WDT) with
     * panic disabled and the idle tasks subscribed. Make a missed feed
     * reset the chip, and watch only our supervisor: SDK work that starves
     * an idle task for a while must not reboot the heating controller. */
    esp_task_wdt_config_t cfg = {
        .timeout_ms     = timeout_ms,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) {
        return OT_PORT_ERR;
    }
    return esp_task_wdt_add(NULL) == ESP_OK ? OT_PORT_OK : OT_PORT_ERR;
}

void wdt_port_feed(void)
{
    esp_task_wdt_reset();
}

/* -------------------------------------------------------------- button */

int btn_port_init(int gpio)
{
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    return gpio_config(&c) == ESP_OK ? OT_PORT_OK : OT_PORT_ERR;
}

bool btn_port_pressed(int gpio)
{
    return gpio_get_level(gpio) == 0;
}

/* -------------------------------------------------------------- system */

const char *sys_port_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external pin";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
        return "task watchdog";
    case ESP_RST_WDT:
        return "other watchdog";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_USB:
        return "usb";
    default:
        return "unknown";
    }
}

bool sys_port_ota_pending_verify(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p && esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

int sys_port_ota_mark_valid(void)
{
    return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK ? OT_PORT_OK : OT_PORT_ERR;
}

const char *sys_port_running_partition(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}
