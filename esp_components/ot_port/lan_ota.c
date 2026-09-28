/**
 * @file lan_ota.c
 * @brief Local HTTP server: firmware update, web UI and remote debugging.
 *
 *   GET  /info  -> {"app":..., "version":..., "partition":...}  (open)
 *   GET  /      status page with forms (app callback)
 *   POST /set   form submit (app callback), then redirect to /
 *   GET  /log   recent log text (app callback)
 *   POST /cli   body = one console command line; output lands in /log
 *   POST /ota   body = application .bin -> other OTA slot, reboot
 *
 * Everything except /info needs either the header X-OTA-Token (scripts) or
 * HTTP Basic auth admin:<web password> (browser).
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
#include "mbedtls/base64.h"

#define CHUNK    4096
#define PAGE_CAP (24 * 1024)
#define FORM_CAP 4096

static httpd_handle_t    s_server;
static lan_log_read_cb_t s_log_cb;
static lan_cli_exec_cb_t s_cli_cb;
static lan_page_cb_t     s_page_cb;
static lan_form_cb_t     s_form_cb;
static char              s_token[65];
static char              s_basic[128]; /* expected "Basic <base64(admin:pass)>" */
static char              s_version[24];

/* constant-time compare of two NUL-terminated strings */
static bool secret_eq(const char *a, const char *b, size_t cap)
{
    uint8_t diff = strlen(a) != strlen(b);
    for (size_t i = 0; i < cap; i++) {
        char ca = i < strlen(a) ? a[i] : 0, cb = i < strlen(b) ? b[i] : 0;
        diff |= (uint8_t)(ca ^ cb);
    }
    return diff == 0;
}

static bool auth_ok(httpd_req_t *req)
{
    char got[sizeof(s_basic)] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", got, sizeof(got)) == ESP_OK &&
        secret_eq(got, s_token, sizeof(s_token))) {
        return true;
    }
    memset(got, 0, sizeof(got));
    return s_basic[0] && httpd_req_get_hdr_value_str(req, "Authorization", got, sizeof(got)) == ESP_OK &&
           secret_eq(got, s_basic, sizeof(s_basic));
}

static bool require_auth(httpd_req_t *req)
{
    if (auth_ok(req)) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"opentherm\"");
    httpd_resp_sendstr(req, "authentication required");
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
    if (!require_auth(req)) {
        return ESP_OK;
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
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char *buf = malloc(PAGE_CAP);
    if (!buf || !s_log_cb) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no log");
        return ESP_FAIL;
    }
    size_t n = s_log_cb(buf, PAGE_CAP);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, buf, n);
    free(buf);
    return ESP_OK;
}

/* Reads the whole body (up to cap-1 bytes) and NUL-terminates it. */
static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    size_t n   = req->content_len < cap - 1 ? req->content_len : cap - 1;
    size_t got = 0;
    while (got < n) {
        int r = httpd_req_recv(req, buf + got, n - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        got += (size_t)r;
    }
    buf[got] = 0;
    return (int)got;
}

static esp_err_t cli_post(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char line[128];
    if (read_body(req, line, sizeof(line)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
        return ESP_FAIL;
    }
    int rc = s_cli_cb ? s_cli_cb(line) : -1;
    httpd_resp_sendstr(req, rc == 0 ? "OK" : "unknown command");
    return ESP_OK;
}

static esp_err_t page_get(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char *buf = malloc(PAGE_CAP);
    if (!buf || !s_page_cb) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no page");
        return ESP_FAIL;
    }
    size_t n = s_page_cb(buf, PAGE_CAP);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, buf, n);
    free(buf);
    return ESP_OK;
}

static esp_err_t form_post(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;
    }
    char *buf = malloc(FORM_CAP);
    if (!buf || read_body(req, buf, FORM_CAP) < 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
        return ESP_FAIL;
    }
    if (s_form_cb) {
        s_form_cb(buf);
    }
    free(buf);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void lan_debug_set_handlers(lan_log_read_cb_t log_cb, lan_cli_exec_cb_t cli_cb)
{
    s_log_cb = log_cb;
    s_cli_cb = cli_cb;
}

void lan_web_set_handlers(lan_page_cb_t page_cb, lan_form_cb_t form_cb)
{
    s_page_cb = page_cb;
    s_form_cb = form_cb;
}

int lan_ota_start(const char *token, uint16_t port, const char *version, const char *web_password)
{
    if (s_server) {
        return OT_PORT_OK;
    }
    if (!token || strlen(token) < 16 || strlen(token) >= sizeof(s_token)) {
        return OT_PORT_ERR; /* refuse to run unprotected */
    }
    strcpy(s_token, token);
    snprintf(s_version, sizeof(s_version), "%s", version ? version : "?");

    /* browser login admin:<web_password>; no web password = no browser access */
    if (web_password && strlen(web_password) >= 8) {
        char   cred[64];
        size_t olen = 0;
        int    n    = snprintf(cred, sizeof(cred), "admin:%s", web_password);
        strcpy(s_basic, "Basic ");
        if (mbedtls_base64_encode((unsigned char *)s_basic + 6, sizeof(s_basic) - 7, &olen, (unsigned char *)cred,
                                  (size_t)n) != 0) {
            s_basic[0] = 0;
        }
    }

    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = port;
    cfg.ctrl_port         = port + 1;
    cfg.recv_wait_timeout = 30;
    cfg.core_id           = 0;
    cfg.stack_size        = 8192;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = NULL;
        return OT_PORT_ERR;
    }

    static const httpd_uri_t uris[] = {
        {.uri = "/info", .method = HTTP_GET, .handler = info_get},
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/set", .method = HTTP_POST, .handler = form_post},
        {.uri = "/log", .method = HTTP_GET, .handler = log_get},
        {.uri = "/cli", .method = HTTP_POST, .handler = cli_post},
        {.uri = "/ota", .method = HTTP_POST, .handler = ota_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    return OT_PORT_OK;
}
