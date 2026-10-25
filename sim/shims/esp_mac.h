/* sim/shims/esp_mac.h — host 桩：设备 MAC。
 * 宿主没有 eFuse，返回固定本地管理位 MAC（02 = locally administered），
 * 同一台机器上设备编号稳定，桥接器据此区分设备。 */
#pragma once

#include "esp_err.h"

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_MAC_WIFI_STA,
    ESP_MAC_WIFI_SOFTAP,
    ESP_MAC_BT,
    ESP_MAC_ETH,
} esp_mac_type_t;

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);

#ifdef __cplusplus
}
#endif
