/* sim/shims/driver/gpio.h — host 桩：BOOT 按钮轮询永远返回松开。 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int gpio_num_t;

#define GPIO_NUM_0  0
#define GPIO_NUM_1  1
#define GPIO_NUM_2  2
#define GPIO_NUM_3  3
#define GPIO_NUM_4  4
#define GPIO_NUM_5  5
#define GPIO_NUM_6  6
#define GPIO_NUM_7  7
#define GPIO_NUM_8  8
#define GPIO_NUM_9  9
#define GPIO_NUM_10 10
#define GPIO_NUM_17 17
#define GPIO_NUM_21 21
#define GPIO_MODE_INPUT  0
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_ONLY   1
#define GPIO_PULLDOWN_ONLY 2
#define GPIO_PULLUP_ENABLE  1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_ENABLE  1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE  0
#define GPIO_INTR_POSEDGE  1
#define GPIO_INTR_NEGEDGE  2

typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;

esp_err_t gpio_config(const gpio_config_t *config);
int gpio_get_level(gpio_num_t gpio_num);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
esp_err_t gpio_reset_pin(gpio_num_t gpio_num);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_set_direction(gpio_num_t gpio_num, int mode);

#ifdef __cplusplus
}
#endif
