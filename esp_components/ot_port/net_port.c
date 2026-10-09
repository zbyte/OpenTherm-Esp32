/**
 * @file net_port.c
 * @brief MQTT client (esp-mqtt) and SNTP for the app; no IDF types exposed.
 *
 * TLS is verified against the IDF certificate bundle. esp-mqtt reconnects by
 * itself; the app only sees connected/disconnected and complete messages.
 *
 * The TuyaOpen sdkconfig builds mbedTLS with MBEDTLS_THREADING_ALT but does
 * not register the mutex functions itself; without them every mbedTLS mutex
 * fails and so does seeding the RNG (ctr_drbg -0x0034). They are
 * registered here before the first TLS connection.
 */

#include "ot_port.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/threading.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "mqtt_client.h"

static const char *TAG = "net_port";

static esp_mqtt_client_handle_t s_client;
static volatile bool            s_connected;
static mqtt_port_conn_cb_t      s_conn_cb;
static mqtt_port_msg_cb_t       s_msg_cb;
static mqtt_port_stats_t        s_stats;

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_stats.connects++;
        if (s_conn_cb) {
            s_conn_cb(true);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected && s_conn_cb) {
            s_conn_cb(false);
        }
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        /* Messages are small (RPC requests); a fragmented one is dropped. */
        if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) {
            ESP_LOGW(TAG, "message of %d bytes too large, dropped", ev->total_data_len);
            break;
        }
        s_stats.rx_msgs++;
        s_stats.rx_bytes += (uint32_t)ev->data_len;
        if (s_msg_cb) {
            s_msg_cb(ev->topic, ev->topic_len, ev->data, ev->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (!ev->error_handle) {
            break;
        }
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "connection refused, code %d (bad token?)", ev->error_handle->connect_return_code);
        } else if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "transport error: esp 0x%x, tls 0x%x, cert flags 0x%x, errno %d",
                     ev->error_handle->esp_tls_last_esp_err, -ev->error_handle->esp_tls_stack_err,
                     ev->error_handle->esp_tls_cert_verify_flags, ev->error_handle->esp_transport_sock_errno);
        }
        break;
    default:
        break;
    }
}

#if defined(MBEDTLS_THREADING_ALT)
static void tls_mutex_init(mbedtls_threading_mutex_t *m)
{
    m->mutex    = xSemaphoreCreateMutex();
    m->is_valid = m->mutex != NULL;
}

static void tls_mutex_free(mbedtls_threading_mutex_t *m)
{
    if (m->is_valid) {
        vSemaphoreDelete(m->mutex);
        m->is_valid = 0;
    }
}

static int tls_mutex_lock(mbedtls_threading_mutex_t *m)
{
    if (!m->is_valid) {
        return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    }
    return xSemaphoreTake(m->mutex, portMAX_DELAY) == pdTRUE ? 0 : MBEDTLS_ERR_THREADING_MUTEX_ERROR;
}

static int tls_mutex_unlock(mbedtls_threading_mutex_t *m)
{
    if (!m->is_valid) {
        return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    }
    return xSemaphoreGive(m->mutex) == pdTRUE ? 0 : MBEDTLS_ERR_THREADING_MUTEX_ERROR;
}
#endif

static void tls_threading_init(void)
{
#if defined(MBEDTLS_THREADING_ALT)
    static bool done;
    if (!done) {
        done = true;
        mbedtls_threading_set_alt(tls_mutex_init, tls_mutex_free, tls_mutex_lock, tls_mutex_unlock);
    }
#endif
}

int mqtt_port_start(const mqtt_port_cfg_t *cfg)
{
    if (s_client) {
        return OT_PORT_BUSY;
    }
    tls_threading_init();
    s_conn_cb = cfg->on_conn;
    s_msg_cb  = cfg->on_msg;

    esp_mqtt_client_config_t c = {
        .broker.address.uri                     = cfg->uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.username                  = cfg->username,
        .credentials.client_id                 = cfg->client_id,
        .session.keepalive                     = cfg->keepalive_s,
        .session.message_retransmit_timeout    = 10000, /* default 1 s floods a slow link */
        .network.reconnect_timeout_ms          = cfg->reconnect_ms,
        .network.timeout_ms                    = 20000,
        .buffer.size                           = 2048,
        .buffer.out_size                       = 2048,
        .task.stack_size                       = 8192,
    };
    s_client = esp_mqtt_client_init(&c);
    if (!s_client) {
        return OT_PORT_ERR;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return OT_PORT_ERR;
    }
    return OT_PORT_OK;
}

void mqtt_port_stop(void)
{
    if (s_client) {
        esp_mqtt_client_destroy(s_client); /* stops the task first */
        s_client = NULL;
    }
    s_connected = false;
}

bool mqtt_port_connected(void)
{
    return s_connected;
}

int mqtt_port_publish(const char *topic, const char *data, int len, int qos)
{
    if (!s_client || !s_connected) {
        return OT_PORT_ERR;
    }
    if (esp_mqtt_client_publish(s_client, topic, data, len, qos, 0) < 0) {
        return OT_PORT_ERR;
    }
    s_stats.tx_msgs++;
    s_stats.tx_bytes += (uint32_t)(len + strlen(topic));
    return OT_PORT_OK;
}

void sys_port_wifi_ps_off(void)
{
    esp_wifi_set_ps(WIFI_PS_NONE);
}

void mqtt_port_get_stats(mqtt_port_stats_t *out)
{
    *out = s_stats;
}

int mqtt_port_subscribe(const char *topic, int qos)
{
    if (!s_client) {
        return OT_PORT_ERR;
    }
    return esp_mqtt_client_subscribe(s_client, topic, qos) < 0 ? OT_PORT_ERR : OT_PORT_OK;
}

static sntp_port_cb_t s_sntp_cb;

static void sntp_synced(struct timeval *tv)
{
    if (s_sntp_cb) {
        s_sntp_cb((uint32_t)tv->tv_sec);
    }
}

int sntp_port_start(const char *server, sntp_port_cb_t on_sync)
{
    static bool started;
    if (started) {
        return OT_PORT_OK;
    }
    s_sntp_cb                 = on_sync;
    esp_sntp_config_t cfg     = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    cfg.sync_cb               = sntp_synced;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        return OT_PORT_ERR;
    }
    started = true;
    return OT_PORT_OK;
}

/* IDF component logs (Wi-Fi, TLS, MQTT) into the app log, so they reach the
 * remote log too. One shared buffer: a line that finds it busy goes to the
 * console only. */
static idf_log_out_t     s_idf_out;
static SemaphoreHandle_t s_idf_lock;
static vprintf_like_t    s_idf_prev;

static int idf_vprintf(const char *fmt, va_list ap)
{
    static char line[256];
    if (!s_idf_lock || xSemaphoreTake(s_idf_lock, 0) != pdTRUE) {
        return s_idf_prev ? s_idf_prev(fmt, ap) : 0;
    }
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    s_idf_out(line);
    xSemaphoreGive(s_idf_lock);
    return n;
}

void sys_port_idf_log_to(idf_log_out_t out, bool verbose_net)
{
    s_idf_out  = out;
    s_idf_lock = xSemaphoreCreateMutex();
    s_idf_prev = esp_log_set_vprintf(idf_vprintf);
    esp_log_level_set("*", ESP_LOG_WARN);
    if (verbose_net) {
        esp_log_level_set("net_port", ESP_LOG_INFO);
        esp_log_level_set("mqtt_client", ESP_LOG_INFO);
        esp_log_level_set("esp-tls", ESP_LOG_INFO);
        esp_log_level_set("esp-tls-mbedtls", ESP_LOG_INFO);
        esp_log_level_set("transport_base", ESP_LOG_INFO);
    }
}
