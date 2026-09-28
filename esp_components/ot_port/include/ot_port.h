/**
 * @file ot_port.h
 * @brief OpenTherm physical layer and ESP-IDF helpers (no IDF types exposed).
 *
 * Levels are logical: ACTIVE/IDLE. A bit is Manchester coded over 1 ms:
 * '1' = ACTIVE then IDLE, '0' = IDLE then ACTIVE. A frame is start bit (1),
 * 32 data bits MSB first, stop bit (1). The *_active_low flags map logical
 * levels to pin levels for the adapter in use.
 */

#ifndef OT_PORT_H_
#define OT_PORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OT_PORT_OK      = 0,
    OT_PORT_TIMEOUT = -1, /* no response started in time */
    OT_PORT_INVALID = -2, /* response started but framing was broken */
    OT_PORT_BUSY    = -3,
    OT_PORT_ERR     = -4,
} ot_port_err_e;

typedef struct {
    int  tx_gpio;
    int  rx_gpio;
    bool tx_active_low;
    bool rx_active_low;
} ot_port_cfg_t;

/* Master side: configures pins and the RX edge interrupt. */
int ot_port_init(const ot_port_cfg_t *cfg);

/* Sends a request frame and waits for the response frame. Blocking. */
int ot_port_transceive(uint32_t request, uint32_t *response, uint32_t timeout_ms);

/*
 * Slave emulator on the SAME pads (internal loopback, no wiring): it listens on
 * the master's TX pad and answers on the master's RX pad. Both pads become
 * input+output. Never enable with a real adapter attached.
 * The handler returns the response frame, or 0 to stay silent.
 */
typedef uint32_t (*ot_port_emu_handler_t)(uint32_t request);
int  ot_port_emu_start(ot_port_emu_handler_t handler, uint32_t reply_delay_ms);
void ot_port_emu_stop(void);
bool ot_port_emu_running(void);

/*
 * 1-Wire bit-banging (open drain, external pull-up). Each time slot runs in a
 * short critical section (<= 70 us, reset 480 us released in between); call
 * from a task on the network core so OpenTherm timing on the other core is
 * never disturbed.
 */
int     ow_port_init(int gpio);
bool    ow_port_reset(int gpio); /* true if a presence pulse was seen */
void    ow_port_write_byte(int gpio, uint8_t byte);
uint8_t ow_port_read_byte(int gpio);

/* Board services (hw_port.c) */
int  led_port_init(int gpio, bool grb); /* single WS2812 via RMT */
void led_port_set(uint8_t r, uint8_t g, uint8_t b);
void led_port_set_raw(uint8_t b0, uint8_t b1, uint8_t b2); /* wire order, for tests */

int  wdt_port_watch_self(uint32_t timeout_ms); /* calling task only, reset on miss */
void wdt_port_feed(void);

int  btn_port_init(int gpio); /* input with pull-up, pressed = LOW */
bool btn_port_pressed(int gpio);

const char *sys_port_reset_reason(void);
/* Record kept in RTC memory across resets (not power loss); take clears it. */
void        sys_port_postmortem_set(uint32_t code, uint32_t uptime_s);
bool        sys_port_postmortem_take(uint32_t *code, uint32_t *uptime_s);
/* Energy counter kept in RTC memory (survives resets, not power loss). */
void        sys_port_energy_set(uint64_t wh);
bool        sys_port_energy_get(uint64_t *wh);
bool        sys_port_ota_pending_verify(void); /* first boot after OTA */
int         sys_port_ota_mark_valid(void);     /* cancel the pending rollback */
void        sys_port_ota_rollback(void);       /* reboot into the previous image */
const char *sys_port_running_partition(void);
bool        sys_port_sta_ip(char *buf, size_t len); /* Wi-Fi station IPv4, false if none */

/* I2C master for on-board sensors (i2c_port.c), 100 kHz, one device at a time. */
int  i2c_port_init(int sda, int scl);
bool i2c_port_probe(uint8_t addr);
int  i2c_port_write(uint8_t addr, const uint8_t *data, size_t len);
int  i2c_port_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len);

/* LAN firmware update (lan_ota.c): HTTP GET /info, POST /ota with header
 * X-OTA-Token. Refuses to start with a token shorter than 16 chars. */
int lan_ota_start(const char *token, uint16_t port, const char *version, const char *web_password);

/* Web UI on the same server: GET / renders a page into out (returns its
 * length); POST /set passes the url-encoded form body, then redirects to /. */
typedef size_t (*lan_page_cb_t)(char *out, size_t cap);
typedef void (*lan_form_cb_t)(const char *body);
void lan_web_set_handlers(lan_page_cb_t page_cb, lan_form_cb_t form_cb);

/* Remote debugging on the same server (token-protected):
 *   GET /log  -> log_cb copies recent log text into buf, returns its length
 *   POST /cli -> cli_cb runs one console line, returns 0 if it was known */
typedef size_t (*lan_log_read_cb_t)(char *buf, size_t cap);
typedef int (*lan_cli_exec_cb_t)(const char *line);
void lan_debug_set_handlers(lan_log_read_cb_t log_cb, lan_cli_exec_cb_t cli_cb);

/* FreeRTOS task pinned to a core (TuyaOpen's thread API has no affinity). */
typedef void (*ot_port_task_fn_t)(void *arg);
int ot_port_task_create(ot_port_task_fn_t fn, const char *name, uint32_t stack_bytes, uint32_t priority, int core,
                        void *arg);

int64_t ot_port_time_us(void);

#ifdef __cplusplus
}
#endif

#endif /* OT_PORT_H_ */
