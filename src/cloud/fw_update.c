/**
 * @file fw_update.c
 * @brief Firmware update from ThingsBoard, see fw_update.h.
 *
 * KV "fw_try" remembers the version being installed across restarts:
 *   {"v":"1.1.2","n":attempts,"i":restarting into it,"b":rolled back}
 * At boot, "i" set but a different version running means the new image was
 * rolled back: that version is marked bad and never downloaded again (a
 * metered link must not loop on a broken image). Downloads that fail are
 * retried at most FW_MAX_TRIES times per version.
 *
 * Some links freeze a TCP connection after a few kilobytes from the server
 * (seen on a home ISP towards ThingsBoard Cloud: one chunk, then silence until
 * the keepalive gives up a minute later). So chunks are small, a lost one is
 * asked again after RETRY_MS, and with no answer by STUCK_MS the connection is
 * dropped and made anew; the download resumes at the same chunk.
 */

#include "fw_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tal_api.h"

#include "cloud.h"
#include "ot_port.h"

#ifndef PROJECT_NAME
#define PROJECT_NAME "opentherm_esp32s3"
#endif
#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

#define TOPIC_TELE    "v1/devices/me/telemetry"
#define TOPIC_REQ     "v2/fw/request/"
#define TOPIC_RESP    "v2/fw/response/"
#define KV_FW         "fw_try"
#define CHUNK         1024   /* small: more of it gets through a link that freezes */
#define RETRY_MS      4000   /* no chunk for this long: ask again */
#define STUCK_MS      8000   /* still nothing: reconnect */
#define STALL_MS      600000 /* no progress for this long: give up */
#define RESTART_MS    3000   /* let the last telemetry go out */
#define FW_MAX_TRIES  3

typedef enum { FW_IDLE, FW_DOWNLOADING, FW_RESTARTING } fw_phase_e;

static struct {
    char     title[48];
    char     version[24];
    char     checksum[132];
    char     alg[12];
    uint32_t size;
} s_target;

static struct {
    char    v[24];
    int     n;
    bool    installing;
    bool    bad;
} s_try;

static MUTEX_HANDLE s_lock;
static fw_phase_e   s_phase;
static uint32_t     s_req; /* request id, new for each download */
static uint32_t     s_chunk, s_got, s_progress;
static uint32_t     s_asked_ms, s_moved_ms, s_restart_ms, s_conn_ms;
static uint32_t     s_reconnects; /* forced during this download */
static char         s_boot_error[64]; /* from the previous attempt, reported once */
static char         s_reported[24];   /* version whose final state went out */
static char         s_status[128] = "idle";

/* ------------------------------------------------------------------ KV */

static void try_save(void)
{
    char json[96];
    snprintf(json, sizeof(json), "{\"v\":\"%s\",\"n\":%d,\"i\":%d,\"b\":%d}", s_try.v, s_try.n, s_try.installing,
             s_try.bad);
    if (tal_kv_set(KV_FW, (const uint8_t *)json, strlen(json)) != OPRT_OK) {
        PR_ERR("fw: saving the update state failed");
    }
}

static void try_load(void)
{
    uint8_t *buf = NULL;
    size_t   len = 0;
    if (tal_kv_get(KV_FW, &buf, &len) != OPRT_OK) {
        return;
    }
    cJSON *o = cJSON_ParseWithLength((const char *)buf, len);
    tal_kv_free(buf);
    cJSON *v = cJSON_GetObjectItem(o, "v");
    if (cJSON_IsString(v)) {
        snprintf(s_try.v, sizeof(s_try.v), "%s", v->valuestring);
        const cJSON *n = cJSON_GetObjectItem(o, "n"), *i = cJSON_GetObjectItem(o, "i");
        const cJSON *b = cJSON_GetObjectItem(o, "b");
        s_try.n          = cJSON_IsNumber(n) ? n->valueint : 0;
        s_try.installing = cJSON_IsNumber(i) && i->valueint;
        s_try.bad        = cJSON_IsNumber(b) && b->valueint;
    }
    cJSON_Delete(o);
}

/* ----------------------------------------------------------- telemetry */

static void send_state(const char *state, const char *error)
{
    char json[192];
    if (error) {
        snprintf(json, sizeof(json), "{\"fw_state\":\"%s\",\"fw_error\":\"%s\"}", state, error);
    } else {
        snprintf(json, sizeof(json), "{\"fw_state\":\"%s\",\"fw_error\":\"\"}", state);
    }
    mqtt_port_publish(TOPIC_TELE, json, (int)strlen(json), 1);
    PR_NOTICE("fw: %s%s%s", state, error ? ": " : "", error ? error : "");
}

static void send_progress(void)
{
    char json[32];
    snprintf(json, sizeof(json), "{\"fw_progress\":%u}", (unsigned)s_progress);
    mqtt_port_publish(TOPIC_TELE, json, (int)strlen(json), 0);
}

/* Final state for a version, once. */
static void report_once(const char *version, const char *state, const char *error)
{
    if (strcmp(s_reported, version) != 0) {
        snprintf(s_reported, sizeof(s_reported), "%s", version);
        send_state(state, error);
    }
}

/* ------------------------------------------------------------ download */

static void ask_chunk(void)
{
    char topic[48], size[8];
    snprintf(topic, sizeof(topic), TOPIC_REQ "%u/chunk/%u", (unsigned)s_req, (unsigned)s_chunk);
    snprintf(size, sizeof(size), "%d", CHUNK);
    /* QoS 0: a lost request is simply asked again after RETRY_MS */
    mqtt_port_publish(topic, size, (int)strlen(size), 0);
    s_asked_ms = tal_system_get_millisecond();
}

static void fail(const char *error)
{
    fw_port_abort();
    s_phase = FW_IDLE;
    snprintf(s_status, sizeof(s_status), "%s failed: %s", s_target.version, error);
    send_state("FAILED", error);
}

static void start(void)
{
    if (strcmp(s_try.v, s_target.version) != 0) {
        snprintf(s_try.v, sizeof(s_try.v), "%s", s_target.version);
        s_try.n   = 0;
        s_try.bad = false;
    }
    s_try.n++;
    s_try.installing = false;
    try_save();

    if (fw_port_begin(s_target.size, s_target.alg) != OT_PORT_OK) {
        fail("cannot start: image too large or checksum algorithm not supported");
        return;
    }
    s_req      = (s_req + 1) % 100000;
    s_chunk    = 0;
    s_got      = 0;
    s_progress = 0;
    s_moved_ms   = tal_system_get_millisecond();
    s_reconnects = 0;
    s_phase      = FW_DOWNLOADING;
    snprintf(s_status, sizeof(s_status), "downloading %s, try %d", s_target.version, s_try.n);
    PR_NOTICE("fw: %s -> %s, %u bytes (try %d)", PROJECT_VERSION, s_target.version, (unsigned)s_target.size,
              s_try.n);
    send_state("DOWNLOADING", NULL);
    ask_chunk();
}

static void finish(void)
{
    send_state("DOWNLOADED", NULL);
    int rc = fw_port_finish(s_target.checksum);
    if (rc == OT_PORT_INVALID) {
        fail("checksum mismatch");
        return;
    }
    if (rc != OT_PORT_OK) {
        fail("image rejected (not an ESP32-S3 application?)");
        return;
    }
    send_state("VERIFIED", NULL);
    s_try.installing = true;
    try_save();
    send_state("UPDATING", NULL);
    snprintf(s_status, sizeof(s_status), "restarting into %s", s_target.version);
    s_phase      = FW_RESTARTING;
    s_restart_ms = tal_system_get_millisecond();
}

/* Decides what to do with the announced package; caller holds the lock. */
static void consider(void)
{
    if (strcmp(s_target.title, PROJECT_NAME) != 0) {
        snprintf(s_status, sizeof(s_status), "package \"%s\" is not for this firmware", s_target.title);
        return;
    }
    if (!strcmp(s_target.version, PROJECT_VERSION)) {
        if (s_phase == FW_DOWNLOADING) {
            fw_port_abort();
            s_phase = FW_IDLE;
        }
        snprintf(s_status, sizeof(s_status), "up to date (%s)", PROJECT_VERSION);
        if (s_try.installing && !strcmp(s_try.v, PROJECT_VERSION)) {
            report_once(PROJECT_VERSION, "UPDATED", NULL);
        }
        return;
    }
    if (s_phase != FW_IDLE) {
        return; /* a download of this version is running; a new one restarts it below */
    }
    if (!strcmp(s_try.v, s_target.version) && s_try.bad) {
        snprintf(s_status, sizeof(s_status), "%s was rolled back, not retried", s_target.version);
        report_once(s_target.version, "FAILED", s_boot_error[0] ? s_boot_error : "rolled back earlier, not retried");
        return;
    }
    if (!strcmp(s_try.v, s_target.version) && s_try.n >= FW_MAX_TRIES) {
        snprintf(s_status, sizeof(s_status), "%s failed %d times, not retried", s_target.version, s_try.n);
        report_once(s_target.version, "FAILED", "gave up after repeated failures");
        return;
    }
    start();
}

/* ------------------------------------------------------------------ API */

void fw_update_init(void)
{
    tal_mutex_create_init(&s_lock);
    try_load();
    if (s_try.installing && strcmp(s_try.v, PROJECT_VERSION) != 0) {
        /* restarted into s_try.v, yet the old image runs: it was rolled back */
        snprintf(s_boot_error, sizeof(s_boot_error), "%s rolled back to %s", s_try.v, PROJECT_VERSION);
        PR_WARN("fw: %s", s_boot_error);
        s_try.installing = false;
        s_try.bad        = true;
        try_save();
    }
    s_req = tal_system_get_random(1000);
}

bool fw_update_on_connect(void)
{
    mqtt_port_subscribe(TOPIC_RESP "+/chunk/+", 1);
    tal_mutex_lock(s_lock);
    s_conn_ms = tal_system_get_millisecond();
    bool busy = s_phase == FW_DOWNLOADING;
    if (busy) {
        ask_chunk(); /* resume where the link dropped */
    }
    tal_mutex_unlock(s_lock);
    if (!busy) {
        char json[128];
        snprintf(json, sizeof(json), "{\"current_fw_title\":\"%s\",\"current_fw_version\":\"%s\"}",
                 PROJECT_NAME, PROJECT_VERSION);
        mqtt_port_publish(TOPIC_TELE, json, (int)strlen(json), 1);
    }
    return busy;
}

void fw_update_on_shared(const cJSON *obj)
{
    const cJSON *title = cJSON_GetObjectItem(obj, "fw_title");
    const cJSON *ver   = cJSON_GetObjectItem(obj, "fw_version");
    const cJSON *size  = cJSON_GetObjectItem(obj, "fw_size");
    const cJSON *sum   = cJSON_GetObjectItem(obj, "fw_checksum");
    const cJSON *alg   = cJSON_GetObjectItem(obj, "fw_checksum_algorithm");
    const cJSON *del   = cJSON_GetObjectItem(obj, "deleted");

    tal_mutex_lock(s_lock);
    if (cJSON_IsArray(del)) {
        const cJSON *k;
        cJSON_ArrayForEach(k, del)
        {
            if (cJSON_IsString(k) && !strcmp(k->valuestring, "fw_version") && s_phase == FW_DOWNLOADING) {
                fw_port_abort();
                s_phase = FW_IDLE;
                snprintf(s_status, sizeof(s_status), "package unassigned, download stopped");
                PR_NOTICE("fw: %s", s_status);
            }
        }
    }
    if (cJSON_IsString(title) && cJSON_IsString(ver) && cJSON_IsNumber(size) && cJSON_IsString(sum)) {
        bool same = !strcmp(s_target.version, ver->valuestring) && !strcmp(s_target.checksum, sum->valuestring);
        snprintf(s_target.title, sizeof(s_target.title), "%s", title->valuestring);
        snprintf(s_target.version, sizeof(s_target.version), "%s", ver->valuestring);
        snprintf(s_target.checksum, sizeof(s_target.checksum), "%s", sum->valuestring);
        snprintf(s_target.alg, sizeof(s_target.alg), "%s", cJSON_IsString(alg) ? alg->valuestring : "SHA256");
        s_target.size = (uint32_t)size->valuedouble;
        if (!same && s_phase == FW_DOWNLOADING) {
            fw_port_abort(); /* another package was assigned meanwhile */
            s_phase = FW_IDLE;
        }
        consider();
    }
    tal_mutex_unlock(s_lock);
}

bool fw_update_on_message(const char *topic, int topic_len, const char *data, int data_len)
{
    const int plen = (int)strlen(TOPIC_RESP);
    if (topic_len <= plen || topic_len > plen + 32 || strncmp(topic, TOPIC_RESP, (size_t)plen) != 0) {
        return false;
    }
    char t[48];
    memcpy(t, topic + plen, (size_t)(topic_len - plen));
    t[topic_len - plen] = 0;
    unsigned req, chunk;
    if (sscanf(t, "%u/chunk/%u", &req, &chunk) != 2) {
        return true;
    }

    tal_mutex_lock(s_lock);
    if (s_phase == FW_DOWNLOADING && req == s_req && chunk == s_chunk) {
        if (data_len <= 0 || s_got + (uint32_t)data_len > s_target.size) {
            fail("server sent an unexpected chunk size");
        } else if (fw_port_write(data, (size_t)data_len) != OT_PORT_OK) {
            fail("flash write failed");
        } else {
            s_got += (uint32_t)data_len;
            s_chunk++;
            s_moved_ms = tal_system_get_millisecond();
            uint32_t pct = (uint32_t)((uint64_t)s_got * 100 / s_target.size);
            if (pct / 10 != s_progress / 10) {
                s_progress = pct;
                send_progress();
                PR_NOTICE("fw: %u%%", (unsigned)pct);
            }
            snprintf(s_status, sizeof(s_status), "downloading %s: %u of %u bytes, %u reconnects", s_target.version,
                     (unsigned)s_got, (unsigned)s_target.size, (unsigned)s_reconnects);
            if (s_got == s_target.size) {
                finish();
            } else {
                ask_chunk();
            }
        }
    }
    tal_mutex_unlock(s_lock);
    return true;
}

/* ms since t; signed, so a stamp taken a moment after `now` reads as 0 */
static int32_t since(uint32_t now, uint32_t t)
{
    int32_t d = (int32_t)(now - t);
    return d > 0 ? d : 0;
}

void fw_update_tick(void)
{
    tal_mutex_lock(s_lock);
    /* read the clock under the lock: the MQTT task may have just started a
     * download and stamped it while this task waited */
    uint32_t now = tal_system_get_millisecond();
    if (s_phase == FW_DOWNLOADING) {
        /* waiting since the last chunk or, if later, since this connection came up */
        int32_t idle = since(now, s_moved_ms) < since(now, s_conn_ms) ? since(now, s_moved_ms)
                                                                       : since(now, s_conn_ms);
        if (since(now, s_moved_ms) >= STALL_MS) {
            fail("download stalled");
        } else if (mqtt_port_connected() && idle >= STUCK_MS) {
            s_reconnects++;
            s_conn_ms = now; /* once per connection */
            PR_DEBUG("fw: no chunk for %d ms, reconnecting (%u)", (int)idle, (unsigned)s_reconnects);
            cloud_reconnect();
        } else if (mqtt_port_connected() && since(now, s_asked_ms) >= RETRY_MS) {
            ask_chunk();
        }
    } else if (s_phase == FW_RESTARTING && since(now, s_restart_ms) >= RESTART_MS) {
        PR_NOTICE("fw: restarting into %s", s_target.version);
        tal_system_sleep(200);
        tal_system_reset();
    }
    tal_mutex_unlock(s_lock);
}

void fw_update_describe(char *buf, size_t cap)
{
    tal_mutex_lock(s_lock);
    snprintf(buf, cap, "%s %s, update: %s", PROJECT_NAME, PROJECT_VERSION, s_status);
    tal_mutex_unlock(s_lock);
}
