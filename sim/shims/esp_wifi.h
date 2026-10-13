/* sim/shims/esp_wifi.h — host 桩：永不连接的假 STA。 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64

#define WIFI_REASON_UNSPECIFIED 1
#define WIFI_REASON_AUTH_EXPIRE 2
#define WIFI_REASON_NO_AP_FOUND 201
#define WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY 202
#define WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD 203
#define WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD 204
#define WIFI_REASON_HANDSHAKE_TIMEOUT 204 + 0
#define WIFI_REASON_CONNECTION_FAIL 3
#define WIFI_REASON_MIC_FAILURE 14
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#define WIFI_REASON_AUTH_FAIL 30
#define WIFI_REASON_ASSOC_FAIL 32

typedef enum { WIFI_MODE_NULL = 0, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
typedef enum { WIFI_IF_STA = 0, WIFI_IF_AP } wifi_interface_t;
typedef enum { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK } wifi_auth_mode_t;

typedef struct {
    uint8_t *ssid;
    uint8_t *password;
} wifi_config_stub_t;

typedef struct {
    uint8_t ssid[WIFI_SSID_MAX_LEN + 1];
    int8_t rssi;
    wifi_auth_mode_t authmode;
} wifi_ap_record_t;

esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_get_mode(wifi_mode_t *mode);
esp_err_t esp_wifi_scan_start(void *config, bool block);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *info);

#ifdef __cplusplus
}
#endif
