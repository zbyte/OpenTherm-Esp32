/**
 * @file lan_ota.c
 * @brief Firmware update and remote debugging over the local network
 *        (HTTP, token-protected).
 *
 *   GET  /info  -> {"app":..., "version":..., "partition":...}  (no token)
 *   POST /ota   body = application .bin -> writes the other OTA slot,
 *               switches the boot slot and restarts
 *   GET  /log   recent log text kept by the app
 *   POST /cli   body = one console command line; output lands in /log
 * All but /info need the header X-OTA-Token.
 *
 * A new image boots "pending verify"; the supervisor confirms it only once
 * control runs healthy and the cloud is reachable, otherwise it rolls back.
 */

#include "ot_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CHUNK   4096
#define LOG_CAP (16 * 1024)

static httpd_handle_t     s_server;
static lan_log_read_cb_t  s_log_cb;
static lan_cli_exec_cb_t  s_cli_cb;
static char               s_token[65];
static char               s_version[24];

static bool token_ok(httpd_req_t *req)
{
    char got[sizeof(s_token)] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", got, sizeof(got)) != ESP_OK) {
        return false;
    }
    /* constant-time compare over the full buffer */
    uint8_t diff = strlen(got) != strlen(s_token);
    for (size_t i = 0; i < sizeof(s_token); i++) {
        diff |= (uint8_t)(got[i] ^ s_token[i]);
    }
    return diff == 0;
}

static bool require_token(httpd_req_t *req)
{
    if (token_ok(req)) {
        return true;
    }
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "bad token");
    return false;
}

static esp_err_t info_get(httpd_req_t *req)
{
    const esp_app_desc_t  *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    char                   buf[160];
    snprintf(buf, sizeof(buf), "{\"app\":\"opentherm_esp32s3\",\"version\":\"%s\",\"idf_app\":\"%s\",\"partition\":\"%s\"}",
             s_version, app->version, run ? run->label : "?");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!require_token(req)) {
        return ESP_FAIL;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len == 0 || req->content_len > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota;
    if (esp_ota_begin(part, req->content_len, &ota) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin");
        return ESP_FAIL;
    }

    char  *buf  = malloc(CHUNK);
    size_t left = req->content_len;
    bool   ok   = buf != NULL;
    while (ok && left > 0) {
        int n = httpd_req_recv(req, buf, left < CHUNK ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        ok = n > 0 && esp_ota_write(ota, buf, n) == ESP_OK;
        left -= ok ? (size_t)n : 0;
    }
    free(buf);

    /* esp_ota_end() validates the image (magic, segments, SHA-256) */
    if (!ok || esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        if (ok) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid image");
        } else {
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        }
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK, restarting\n");
    xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t log_get(httpd_req_t *req)
{
    if (!require_token(req)) {
        return ESP_FAIL;
    }
    char *buf = malloc(LOG_CAP);
    if (!buf || !s_log_cb) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no log");
        return ESP_FAIL;
    }
    size_t n = s_log_cb(buf, LOG_CAP);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, buf, n);
    free(buf);
    return ESP_OK;
}

static esp_err_t cli_post(httpd_req_t *req)
{
    if (!require_token(req)) {
        return ESP_FAIL;
    }
    char   line[128];
    size_t n   = req->content_len < sizeof(line) - 1 ? req->content_len : sizeof(line) - 1;
    size_t got = 0;
    while (got < n) {
        int r = httpd_req_recv(req, line + got, n - got);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    line[got] = 0;
    int rc    = s_cli_cb ? s_cli_cb(line) : -1;
    httpd_resp_sendstr(req, rc == 0 ? "OK" : "unknown command");
    return ESP_OK;
}

void lan_debug_set_handlers(lan_log_read_cb_t log_cb, lan_cli_exec_cb_t cli_cb)
{
    s_log_cb = log_cb;
    s_cli_cb = cli_cb;
}

int lan_ota_start(const char *token, uint16_t port, const char *version)
{
    if (s_server) {
        return OT_PORT_OK;
    }
    if (!token || strlen(token) < 16 || strlen(token) >= sizeof(s_token)) {
        return OT_PORT_ERR; /* refuse to run unprotected */
    }
    strcpy(s_token, token);
    snprintf(s_version, sizeof(s_version), "%s", version ? version : "?");

    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = port;
    cfg.ctrl_port         = port + 1;
    cfg.recv_wait_timeout = 30;
    cfg.core_id           = 0;
    cfg.stack_size        = 6144;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = NULL;
        return OT_PORT_ERR;
    }

    static const httpd_uri_t info = {.uri = "/info", .method = HTTP_GET, .handler = info_get};
    static const httpd_uri_t ota  = {.uri = "/ota", .method = HTTP_POST, .handler = ota_post};
    static const httpd_uri_t log  = {.uri = "/log", .method = HTTP_GET, .handler = log_get};
    static const httpd_uri_t cli  = {.uri = "/cli", .method = HTTP_POST, .handler = cli_post};
    httpd_register_uri_handler(s_server, &info);
    httpd_register_uri_handler(s_server, &ota);
    httpd_register_uri_handler(s_server, &log);
    httpd_register_uri_handler(s_server, &cli);
    return OT_PORT_OK;
}
