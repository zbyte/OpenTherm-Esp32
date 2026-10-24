/**
 * @file fw_port.c
 * @brief Firmware image written piece by piece (OTA from the cloud).
 *
 * The image goes into the next OTA partition as it arrives, erased sector by
 * sector (no long erase up front that would stall the other core), and is
 * hashed on the way. finish() checks the hash the server announced, lets
 * esp_ota_end() validate the image itself and makes it the boot partition;
 * the new firmware then confirms itself or the bootloader rolls it back.
 *
 * A package may also be the image zlib-compressed (about a third smaller on a
 * metered link): an ESP image starts with 0xE9, a zlib stream with 0x78. It is
 * unpacked on the way with the inflater in ROM through a 32 KB window; the
 * checksum covers the package as sent, zlib's Adler-32 the unpacked image.
 */

#include "ot_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mbedtls/md.h"
#include "miniz.h"

static const char *TAG = "fw";

static esp_ota_handle_t        s_ota;
static const esp_partition_t  *s_part;
static mbedtls_md_context_t    s_md;
static bool                    s_active;
static uint32_t                s_size, s_done;
static size_t                  s_md_len;

/* compressed package */
typedef enum { PKG_UNKNOWN, PKG_IMAGE, PKG_ZLIB } pkg_e;
static pkg_e               s_pkg;
static tinfl_decompressor *s_inf;
static uint8_t            *s_win; /* TINFL_LZ_DICT_SIZE, wraps */
static size_t              s_win_pos;
static bool                s_inf_done;

static void inflate_free(void)
{
    free(s_inf);
    free(s_win);
    s_inf = NULL;
    s_win = NULL;
}

/* Unpacks len bytes into the OTA partition; last: nothing follows. */
static int inflate_write(const uint8_t *in, size_t len, bool last)
{
    const mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (last ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
    for (;;) {
        size_t       in_n  = len;
        size_t       out_n = TINFL_LZ_DICT_SIZE - s_win_pos;
        tinfl_status st    = tinfl_decompress(s_inf, in, &in_n, s_win, s_win + s_win_pos, &out_n, flags);
        in += in_n;
        len -= in_n;
        if (out_n && esp_ota_write(s_ota, s_win + s_win_pos, out_n) != ESP_OK) {
            return OT_PORT_ERR;
        }
        s_win_pos = (s_win_pos + out_n) & (TINFL_LZ_DICT_SIZE - 1);
        if (st < TINFL_STATUS_DONE) {
            ESP_LOGW(TAG, "bad compressed data (%d)", (int)st);
            return OT_PORT_ERR;
        }
        if (st == TINFL_STATUS_DONE) {
            s_inf_done = true;
            return len ? OT_PORT_ERR : OT_PORT_OK; /* nothing may follow the stream */
        }
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && !len) {
            return last ? OT_PORT_ERR : OT_PORT_OK;
        }
    }
}

static mbedtls_md_type_t md_type(const char *alg)
{
    static const struct {
        const char       *name;
        mbedtls_md_type_t type;
    } algs[] = {{"SHA256", MBEDTLS_MD_SHA256}, {"SHA384", MBEDTLS_MD_SHA384},
                {"SHA512", MBEDTLS_MD_SHA512}, {"MD5", MBEDTLS_MD_MD5}};
    for (size_t i = 0; alg && i < sizeof(algs) / sizeof(algs[0]); i++) {
        if (!strcasecmp(alg, algs[i].name)) {
            return algs[i].type;
        }
    }
    return MBEDTLS_MD_NONE;
}

void fw_port_abort(void)
{
    if (s_active) {
        esp_ota_abort(s_ota);
        mbedtls_md_free(&s_md);
        s_active = false;
    }
    inflate_free();
}

int fw_port_begin(uint32_t size, const char *checksum_alg)
{
    fw_port_abort();
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md_type(checksum_alg));
    if (!info) {
        ESP_LOGW(TAG, "checksum algorithm %s not supported", checksum_alg ? checksum_alg : "?");
        return OT_PORT_ERR;
    }
    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part || !size || size > s_part->size) {
        ESP_LOGW(TAG, "image of %u bytes does not fit", (unsigned)size);
        return OT_PORT_ERR;
    }
    if (esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota) != ESP_OK) {
        return OT_PORT_ERR;
    }
    mbedtls_md_init(&s_md);
    if (mbedtls_md_setup(&s_md, info, 0) != 0 || mbedtls_md_starts(&s_md) != 0) {
        mbedtls_md_free(&s_md);
        esp_ota_abort(s_ota);
        return OT_PORT_ERR;
    }
    s_md_len = mbedtls_md_get_size(info);
    s_size   = size;
    s_done   = 0;
    s_pkg    = PKG_UNKNOWN;
    s_active = true;
    return OT_PORT_OK;
}

int fw_port_write(const void *data, size_t len)
{
    if (!s_active || s_done + len > s_size) {
        return OT_PORT_ERR;
    }
    if (s_pkg == PKG_UNKNOWN && len) {
        s_pkg = ((const uint8_t *)data)[0] == 0x78 ? PKG_ZLIB : PKG_IMAGE;
        if (s_pkg == PKG_ZLIB) {
            s_inf = malloc(sizeof(tinfl_decompressor));
            s_win = malloc(TINFL_LZ_DICT_SIZE);
            if (!s_inf || !s_win) {
                ESP_LOGW(TAG, "no memory to unpack the image");
                inflate_free();
                return OT_PORT_ERR;
            }
            tinfl_init(s_inf);
            s_win_pos  = 0;
            s_inf_done = false;
            ESP_LOGI(TAG, "compressed package");
        }
    }
    if (mbedtls_md_update(&s_md, data, len) != 0) {
        return OT_PORT_ERR;
    }
    s_done += (uint32_t)len;
    if (s_pkg == PKG_ZLIB) {
        return inflate_write(data, len, s_done == s_size);
    }
    return esp_ota_write(s_ota, data, len) == ESP_OK ? OT_PORT_OK : OT_PORT_ERR;
}

int fw_port_finish(const char *checksum_hex)
{
    if (!s_active || s_done != s_size || (s_pkg == PKG_ZLIB && !s_inf_done)) {
        fw_port_abort();
        return OT_PORT_ERR;
    }
    inflate_free();
    unsigned char sum[64];
    char          hex[129];
    mbedtls_md_finish(&s_md, sum);
    mbedtls_md_free(&s_md);
    for (size_t i = 0; i < s_md_len; i++) {
        snprintf(hex + 2 * i, 3, "%02x", sum[i]);
    }
    if (!checksum_hex || strcasecmp(hex, checksum_hex) != 0) {
        ESP_LOGW(TAG, "checksum mismatch");
        esp_ota_abort(s_ota);
        s_active = false;
        return OT_PORT_INVALID;
    }
    s_active = false;
    /* esp_ota_end() validates the image (magic, segments, its own SHA-256) */
    if (esp_ota_end(s_ota) != ESP_OK || esp_ota_set_boot_partition(s_part) != ESP_OK) {
        return OT_PORT_ERR;
    }
    return OT_PORT_OK;
}
