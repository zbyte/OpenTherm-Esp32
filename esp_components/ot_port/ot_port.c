/**
 * @file ot_port.c
 * @brief OpenTherm physical layer on ESP-IDF.
 *
 * Receiver: edge interrupt + esp_timer timestamps, state machine after
 * ihormelnyk/opentherm_library: an edge more than 750 us after the previous
 * mid-bit edge is itself a mid-bit edge and carries the bit value.
 * Transmitter: the RMT peripheral plays the whole frame (34 Manchester bits
 * fit its 48-symbol memory, no refills). A software loop stalls whenever a
 * flash write freezes the cores (KV saves, firmware download): the frame then
 * went out broken and a BAXI Ampera took such garbage for writes to its
 * service parameters (power limit 5.0 -> 0, pump 11.0 -> 3). The busy-wait
 * loop is kept for the loopback emulator and as a fallback.
 */

#include "ot_port.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "soc/soc_caps.h"
#include "soc/gpio_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define HALF_BIT_US       500
#define MID_BIT_MIN_US    750  /* edges closer than this are bit-boundary edges */
#define START_BIT_MAX_US  1150 /* start bit's first half must be shorter */
#define MIN_FRAME_GAP_US  100000

typedef enum {
    RX_IDLE,
    RX_WAIT_START, /* waiting for the first ACTIVE edge */
    RX_START_BIT,  /* in the first half of the start bit */
    RX_BITS,
    RX_DONE,
    RX_ERROR,
} rx_state_e;

typedef struct {
    int                        gpio;
    bool                       active_low;
    volatile rx_state_e        state;
    volatile int64_t           mid_ts;
    volatile uint32_t          frame;
    volatile uint8_t           bits;
    SemaphoreHandle_t          done;
} rx_ctx_t;

static ot_port_cfg_t s_cfg;
static bool          s_inited;
static rx_ctx_t      s_master_rx; /* master listens on cfg.rx_gpio */
static rx_ctx_t      s_emu_rx;    /* emulator listens on cfg.tx_gpio */
static int64_t       s_last_frame_end;
static volatile uint32_t s_rx_edges; /* master RX, diagnostics */

static rmt_channel_handle_t s_tx_chan; /* NULL: software transmitter */
static rmt_encoder_handle_t s_tx_enc;
static SemaphoreHandle_t    s_tx_lock; /* held while a frame is on the line */

static ot_port_emu_handler_t s_emu_handler;
static uint32_t              s_emu_delay_ms;
static TaskHandle_t          s_emu_task;
static volatile bool         s_emu_run;

/* ---------------------------------------------------------------- levels */

/* Called from the RX interrupt, which also runs while a flash write has the
 * cache disabled: gpio_get_level() lives in flash and hung the core there
 * (interrupt watchdog) whenever the boiler answered during a KV save. */
static inline bool IRAM_ATTR pin_is_active(int gpio, bool active_low)
{
    return gpio_ll_get_level(&GPIO, gpio) != (active_low ? 1 : 0);
}

static inline void set_active(int gpio, bool active_low, bool active)
{
    gpio_set_level(gpio, (active != active_low) ? 1 : 0);
}

/* ------------------------------------------------------------------- RX */

static void IRAM_ATTR rx_isr(void *arg)
{
    rx_ctx_t *rx     = (rx_ctx_t *)arg;
    int64_t   now    = esp_timer_get_time();
    bool      active = pin_is_active(rx->gpio, rx->active_low);
    BaseType_t woken = pdFALSE;

    if (rx == &s_master_rx) {
        s_rx_edges = s_rx_edges + 1;
    }

    switch (rx->state) {
    case RX_WAIT_START:
        if (active) {
            rx->state  = RX_START_BIT;
            rx->mid_ts = now;
        }
        break;

    case RX_START_BIT:
        if (!active && now - rx->mid_ts < START_BIT_MAX_US) {
            rx->state  = RX_BITS;
            rx->mid_ts = now;
            rx->frame  = 0;
            rx->bits   = 0;
        } else {
            rx->state = RX_ERROR;
            xSemaphoreGiveFromISR(rx->done, &woken);
        }
        break;

    case RX_BITS:
        if (now - rx->mid_ts <= MID_BIT_MIN_US) {
            break; /* bit-boundary edge */
        }
        if (rx->bits < 32) {
            rx->frame  = (rx->frame << 1) | (active ? 0u : 1u);
            rx->bits   = rx->bits + 1;
            rx->mid_ts = now;
        } else {
            /* stop bit is a '1': its mid-bit edge goes ACTIVE -> IDLE */
            rx->state = active ? RX_ERROR : RX_DONE;
            xSemaphoreGiveFromISR(rx->done, &woken);
        }
        break;

    default:
        break;
    }

    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static void rx_arm(rx_ctx_t *rx)
{
    xSemaphoreTake(rx->done, 0);
    rx->state = RX_WAIT_START;
}

/* Waits for a complete frame; a frame already in progress may finish late,
 * but only once: a lone noise edge leaves the state machine "in a frame"
 * forever, and waiting on it must not hang the caller. */
static int rx_wait(rx_ctx_t *rx, uint32_t start_timeout_ms, uint32_t *frame)
{
    if (xSemaphoreTake(rx->done, pdMS_TO_TICKS(start_timeout_ms)) != pdTRUE) {
        bool in_frame = rx->state == RX_START_BIT || rx->state == RX_BITS;
        /* a frame lasts 34 ms; allow it to complete, then give up */
        if (!in_frame || xSemaphoreTake(rx->done, pdMS_TO_TICKS(60)) != pdTRUE) {
            rx->state = RX_IDLE;
            return in_frame ? OT_PORT_INVALID : OT_PORT_TIMEOUT;
        }
    }

    rx_state_e st = rx->state;
    rx->state     = RX_IDLE;
    if (st != RX_DONE) {
        return OT_PORT_INVALID;
    }
    *frame = rx->frame;
    return OT_PORT_OK;
}

/* ------------------------------------------------------------------- TX */

static void tx_frame(int gpio, bool active_low, uint32_t frame)
{
    int64_t t = esp_timer_get_time();

    for (int i = 0; i < 34; i++) {
        bool bit = (i == 0 || i == 33) ? true : ((frame >> (31 - (i - 1))) & 1u);

        set_active(gpio, active_low, bit);
        t += HALF_BIT_US;
        while (esp_timer_get_time() < t) {
        }
        set_active(gpio, active_low, !bit);
        t += HALF_BIT_US;
        while (esp_timer_get_time() < t) {
        }
    }
    set_active(gpio, active_low, false);
}

static int tx_rmt_init(int gpio, bool active_low)
{
    rmt_tx_channel_config_t ch = {
        .gpio_num          = gpio,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 1000000, /* 1 us ticks */
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
        .trans_queue_depth = 1,
        .flags =
            {
                .invert_out   = active_low,
                .io_loop_back = true, /* the loopback emulator listens on this pad */
            },
    };
    rmt_copy_encoder_config_t enc = {};
    if (rmt_new_tx_channel(&ch, &s_tx_chan) != ESP_OK) {
        s_tx_chan = NULL;
        return OT_PORT_ERR;
    }
    if (rmt_new_copy_encoder(&enc, &s_tx_enc) != ESP_OK || rmt_enable(s_tx_chan) != ESP_OK) {
        rmt_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return OT_PORT_ERR;
    }
    return OT_PORT_OK;
}

static int tx_frame_rmt(uint32_t frame)
{
    static rmt_symbol_word_t sym[34]; /* must stay valid until the frame is out */
    for (int i = 0; i < 34; i++) {
        bool bit = (i == 0 || i == 33) ? true : ((frame >> (31 - (i - 1))) & 1u);
        sym[i]   = (rmt_symbol_word_t){
              .duration0 = HALF_BIT_US, .level0 = bit, .duration1 = HALF_BIT_US, .level1 = !bit};
    }
    rmt_transmit_config_t tc = {.loop_count = 0, .flags.eot_level = 0};
    if (rmt_transmit(s_tx_chan, s_tx_enc, sym, sizeof(sym), &tc) != ESP_OK) {
        return OT_PORT_ERR;
    }
    /* 34 ms on the line; the done interrupt may come late during a flash write */
    return rmt_tx_wait_all_done(s_tx_chan, 500) == ESP_OK ? OT_PORT_OK : OT_PORT_ERR;
}

/* Before any restart: let the frame on the line finish and start no other,
 * a cut frame is garbage to the boiler. */
static void ot_shutdown(void)
{
    if (s_tx_lock) {
        xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(600));
    }
}

/* --------------------------------------------------------------- master */

static int rx_setup(rx_ctx_t *rx, int gpio, bool active_low)
{
    rx->gpio       = gpio;
    rx->active_low = active_low;
    rx->state      = RX_IDLE;
    if (!rx->done) {
        rx->done = xSemaphoreCreateBinary();
    }
    gpio_set_intr_type(gpio, GPIO_INTR_ANYEDGE);
    esp_err_t err = gpio_isr_handler_add(gpio, rx_isr, rx);
    return err == ESP_OK ? OT_PORT_OK : OT_PORT_ERR;
}

int ot_port_init(const ot_port_cfg_t *cfg)
{
    if (s_inited) {
        return OT_PORT_OK;
    }
    s_cfg = *cfg;

    gpio_config_t out = {
        .pin_bit_mask = 1ULL << cfg->tx_gpio,
        /* input enabled too so the loopback emulator can listen on this pad */
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    gpio_config(&out);
    set_active(cfg->tx_gpio, cfg->tx_active_low, false);

    /* Weak pull towards the idle level: an unconnected RX pin (bare module,
     * unplugged adapter) must not pick up noise edges. The adapter board has
     * its own 1.5k pull-down (R5), which dominates. */
    gpio_config_t in = {
        .pin_bit_mask   = 1ULL << cfg->rx_gpio,
        .mode           = GPIO_MODE_INPUT,
        .pull_up_en     = cfg->rx_active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en   = cfg->rx_active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&in);

    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return OT_PORT_ERR;
    }
    if (rx_setup(&s_master_rx, cfg->rx_gpio, cfg->rx_active_low) != OT_PORT_OK) {
        return OT_PORT_ERR;
    }

    s_tx_lock = xSemaphoreCreateMutex();
    if (tx_rmt_init(cfg->tx_gpio, cfg->tx_active_low) != OT_PORT_OK) {
        esp_rom_printf("ot_port: RMT transmitter unavailable, software timing\n");
    }
    esp_register_shutdown_handler(ot_shutdown);

    s_inited = true;
    return OT_PORT_OK;
}

int ot_port_transceive(uint32_t request, uint32_t *response, uint32_t timeout_ms)
{
    if (!s_inited) {
        return OT_PORT_ERR;
    }

    int64_t gap = s_last_frame_end + MIN_FRAME_GAP_US - esp_timer_get_time();
    if (gap > 0) {
        vTaskDelay(pdMS_TO_TICKS(gap / 1000 + 1));
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    rx_arm(&s_master_rx);
    int tx = OT_PORT_OK;
    if (s_tx_chan) {
        tx = tx_frame_rmt(request);
    } else {
        tx_frame(s_cfg.tx_gpio, s_cfg.tx_active_low, request);
    }
    xSemaphoreGive(s_tx_lock);
    if (tx != OT_PORT_OK) {
        s_master_rx.state = RX_IDLE;
        s_last_frame_end  = esp_timer_get_time();
        return OT_PORT_ERR;
    }
    int rc = rx_wait(&s_master_rx, timeout_ms, response);
    s_last_frame_end = esp_timer_get_time();
    return rc;
}

uint32_t ot_port_rx_edges(void)
{
    return s_rx_edges;
}

bool ot_port_rx_active(void)
{
    return s_inited && pin_is_active(s_cfg.rx_gpio, s_cfg.rx_active_low);
}

/* ------------------------------------------------------------- emulator */

static void emu_task(void *arg)
{
    while (s_emu_run) {
        uint32_t req;
        rx_arm(&s_emu_rx);
        int rc = rx_wait(&s_emu_rx, 200, &req);
        if (rc != OT_PORT_OK || !s_emu_run) {
            continue;
        }
        uint32_t resp = s_emu_handler(req);
        if (resp) {
            vTaskDelay(pdMS_TO_TICKS(s_emu_delay_ms));
            /* emulator drives the master's RX pad with the RX polarity */
            tx_frame(s_cfg.rx_gpio, s_cfg.rx_active_low, resp);
        }
    }
    s_emu_rx.state = RX_IDLE;
    s_emu_task     = NULL;
    vTaskDelete(NULL);
}

int ot_port_emu_start(ot_port_emu_handler_t handler, uint32_t reply_delay_ms)
{
    if (!s_inited || s_emu_task) {
        return OT_PORT_BUSY;
    }

    gpio_set_direction(s_cfg.rx_gpio, GPIO_MODE_INPUT_OUTPUT);
    set_active(s_cfg.rx_gpio, s_cfg.rx_active_low, false);
    if (rx_setup(&s_emu_rx, s_cfg.tx_gpio, s_cfg.tx_active_low) != OT_PORT_OK) {
        return OT_PORT_ERR;
    }

    s_emu_handler  = handler;
    s_emu_delay_ms = reply_delay_ms;
    s_emu_run      = true;
    /* Other core than the master so both sides can busy-wait independently. */
    if (xTaskCreatePinnedToCore(emu_task, "ot_emu", 4096, NULL, 19, &s_emu_task, 0) != pdPASS) {
        s_emu_run = false;
        return OT_PORT_ERR;
    }
    return OT_PORT_OK;
}

void ot_port_emu_stop(void)
{
    if (!s_emu_task) {
        return;
    }
    s_emu_run = false;
    while (s_emu_task) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    gpio_isr_handler_remove(s_cfg.tx_gpio);
    gpio_set_direction(s_cfg.rx_gpio, GPIO_MODE_INPUT);
}

bool ot_port_emu_running(void)
{
    return s_emu_task != NULL;
}

/* --------------------------------------------------------------- 1-Wire */

/* Register access from IRAM: gpio_set_level() runs from flash, and a cache
 * miss inside a slot pushed the read sample past the 15 us the sensor
 * guarantees to hold a '0'. */
static portMUX_TYPE s_ow_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void IRAM_ATTR ow_set(int gpio, int level)
{
    gpio_ll_set_level(&GPIO, gpio, level);
}

static inline int IRAM_ATTR ow_get(int gpio)
{
    return gpio_ll_get_level(&GPIO, gpio);
}

int ow_port_init(int gpio)
{
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        /* The board has 4.7k pull-ups; the weak internal one keeps an
         * unconnected pin from reading as a bus held low. */
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&c) != ESP_OK) {
        return OT_PORT_ERR;
    }
    gpio_set_level(gpio, 1);
    return OT_PORT_OK;
}

void ow_port_strong_pullup(int gpio)
{
    gpio_set_level(gpio, 1);
    gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT);
}

static IRAM_ATTR bool ow_presence(int gpio)
{
    bool present = false;

    portENTER_CRITICAL(&s_ow_mux);
    ow_set(gpio, 1);
    /* the sensor waits 15-60 us, then holds the bus low for 60-240 us */
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < 240) {
        if (!ow_get(gpio)) {
            present = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_ow_mux);
    return present;
}

ow_reset_e ow_port_reset(int gpio)
{
    gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT_OD); /* end strong pull-up */
    ow_set(gpio, 1);
    esp_rom_delay_us(5);
    if (!ow_get(gpio)) {
        return OW_STUCK_LOW;
    }

    ow_set(gpio, 0);
    esp_rom_delay_us(480);
    bool present = ow_presence(gpio);
    esp_rom_delay_us(480);
    return present ? OW_PRESENT : OW_NO_PRESENCE;
}

static IRAM_ATTR void ow_write_bit(int gpio, bool bit)
{
    portENTER_CRITICAL(&s_ow_mux);
    ow_set(gpio, 0);
    esp_rom_delay_us(bit ? 5 : 60);
    ow_set(gpio, 1);
    portEXIT_CRITICAL(&s_ow_mux);
    esp_rom_delay_us(bit ? 65 : 10);
}

bool IRAM_ATTR ow_port_read_bit(int gpio)
{
    portENTER_CRITICAL(&s_ow_mux);
    ow_set(gpio, 0);
    esp_rom_delay_us(2);
    ow_set(gpio, 1);
    esp_rom_delay_us(8); /* sample ~10 us after the falling edge (limit 15) */
    bool bit = ow_get(gpio);
    portEXIT_CRITICAL(&s_ow_mux);
    esp_rom_delay_us(60);
    return bit;
}

void ow_port_write_byte(int gpio, uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(gpio, (byte >> i) & 1u);
    }
}

uint8_t ow_port_read_byte(int gpio)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint8_t)(ow_port_read_bit(gpio) << i);
    }
    return v;
}

/* -------------------------------------------------------------- helpers */

int ot_port_task_create(ot_port_task_fn_t fn, const char *name, uint32_t stack_bytes, uint32_t priority, int core,
                        void *arg)
{
    return xTaskCreatePinnedToCore(fn, name, stack_bytes, arg, priority, NULL, core) == pdPASS ? OT_PORT_OK
                                                                                                : OT_PORT_ERR;
}

int64_t ot_port_time_us(void)
{
    return esp_timer_get_time();
}
