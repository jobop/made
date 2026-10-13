/* sim/shims/esp_err.h — host 桩：esp_err_t 与常用错误码。 */
#pragma once

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK                         0
#define ESP_FAIL                       -1
#define ESP_ERR_NO_MEM                 0x101
#define ESP_ERR_INVALID_ARG            0x102
#define ESP_ERR_INVALID_STATE          0x103
#define ESP_ERR_INVALID_SIZE           0x104
#define ESP_ERR_NOT_FOUND              0x105
#define ESP_ERR_NOT_SUPPORTED          0x106
#define ESP_ERR_TIMEOUT                0x107
#define ESP_ERR_INVALID_RESPONSE       0x108
#define ESP_ERR_INVALID_CRC            0x109
#define ESP_ERR_NVS_NO_FREE_PAGES      0x110
#define ESP_ERR_NVS_NEW_VERSION_FOUND  0x111
#define ESP_ERR_NVS_NOT_FOUND          0x112
#define ESP_ERR_NVS_INVALID_NAME       0x113

const char *esp_err_to_name(esp_err_t code);

#ifdef ESP_ERROR_CHECK
#undef ESP_ERROR_CHECK
#endif
#define ESP_ERROR_CHECK(x) do { \
    esp_err_t err_rc_ = (x); \
    if (err_rc_ != ESP_OK) { \
        fprintf(stderr, "ESP_ERROR_CHECK failed: %s (%s:%d)\n", \
                esp_err_to_name(err_rc_), __FILE__, __LINE__); \
    } \
} while (0)

#ifdef __cplusplus
}
#endif
