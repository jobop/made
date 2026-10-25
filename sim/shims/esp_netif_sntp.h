/* sim/shims/esp_netif_sntp.h — host 桩：SNTP。
 * 宿主系统时钟本身就是可信的（sim 由 macOS/安卓由系统维护），
 * init 即视为同步完成，HTTPS 证书时间校验直接放行。 */
#pragma once

#include "esp_err.h"

#include <cstdint>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *servers[1];
    int start;
    void (*sync_cb)(struct timeval *tv);
} esp_sntp_config_t;

#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) \
    { {server}, 0 }

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);

#ifdef __cplusplus
}
#endif
