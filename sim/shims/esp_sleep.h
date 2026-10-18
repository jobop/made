/* sim/shims/esp_sleep.h — host 桩。模拟器不进入深度睡眠。 */
#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_SLEEP_WAKEUP_ALL = 0,
} esp_sleep_source_t;

esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_source_t source);
esp_err_t esp_sleep_enable_ext0_wakeup(gpio_num_t gpio, int level);
void esp_deep_sleep_start(void);

#ifdef __cplusplus
}
#endif
