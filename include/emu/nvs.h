#pragma once

/*
 * ESP-IDF's NVS calls over the settings file (emu/settings.h), so the
 * machines' settings.c from the esp32* projects carry over as they are: a
 * namespace is a section, every value a line. Integers as numbers, strings
 * as they are, blobs as hex.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu/settings.h"

typedef int esp_err_t;
typedef const char *nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

#ifndef ESP_OK
#define ESP_OK                  0
#define ESP_FAIL                -1
#endif
#define ESP_ERR_NVS_NOT_FOUND   0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c

static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    (void)mode;
    *h = ns;
    return ESP_OK;
}

static inline void nvs_close(nvs_handle_t h)
{
    (void)h;
}

static inline esp_err_t nvs_commit(nvs_handle_t h)
{
    (void)h;
    return ESP_OK;
}

static inline esp_err_t nvs_get_i64_(nvs_handle_t h, const char *key, int64_t *v)
{
    char text[32], *end;
    if (!emu_setting_get_str(h, key, text, sizeof(text))) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    long long x = strtoll(text, &end, 0);
    if (end == text) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *v = x;
    return ESP_OK;
}

static inline esp_err_t nvs_set_i64_(nvs_handle_t h, const char *key, int64_t v)
{
    char text[32];
    snprintf(text, sizeof(text), "%lld", (long long)v);
    emu_setting_set_str(h, key, text);
    return ESP_OK;
}

#define NVS_INT(suffix, type)                                                              \
    static inline esp_err_t nvs_get_##suffix(nvs_handle_t h, const char *key, type *out)  \
    {                                                                                      \
        int64_t v;                                                                         \
        esp_err_t err = nvs_get_i64_(h, key, &v);                                          \
        if (err == ESP_OK) {                                                               \
            *out = (type)v;                                                                \
        }                                                                                  \
        return err;                                                                        \
    }                                                                                      \
    static inline esp_err_t nvs_set_##suffix(nvs_handle_t h, const char *key, type v)     \
    {                                                                                      \
        return nvs_set_i64_(h, key, (int64_t)v);                                           \
    }

NVS_INT(u8, uint8_t)
NVS_INT(i8, int8_t)
NVS_INT(u16, uint16_t)
NVS_INT(i16, int16_t)
NVS_INT(u32, uint32_t)
NVS_INT(i32, int32_t)
NVS_INT(u64, uint64_t)
NVS_INT(i64, int64_t)

/* As NVS: *len in, the size of buf (0 / out NULL: just the length wanted);
 * *len out, with the 0
 */
static inline esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    char text[2048];
    if (!emu_setting_get_str(h, key, text, sizeof(text))) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    size_t need = strlen(text) + 1;
    if (out == NULL) {
        *len = need;
        return ESP_OK;
    }
    if (*len < need) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, text, need);
    *len = need;
    return ESP_OK;
}

static inline esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    emu_setting_set_str(h, key, value);
    return ESP_OK;
}

static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    char text[2048];
    if (!emu_setting_get_str(h, key, text, sizeof(text))) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    size_t n = strlen(text) / 2;
    if (out == NULL) {
        *len = n;
        return ESP_OK;
    }
    if (*len < n) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(text + 2 * i, "%2x", &v);
        ((uint8_t *)out)[i] = (uint8_t)v;
    }
    *len = n;
    return ESP_OK;
}

static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len)
{
    char text[2048];
    size_t n = 0;
    for (size_t i = 0; i < len && n + 3 < sizeof(text); i++) {
        n += snprintf(text + n, sizeof(text) - n, "%02x", ((const uint8_t *)value)[i]);
    }
    text[n] = 0;
    emu_setting_set_str(h, key, text);
    return ESP_OK;
}

static inline esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    emu_setting_erase(h, key);
    return ESP_OK;
}
