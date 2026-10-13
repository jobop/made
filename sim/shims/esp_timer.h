/* sim/shims/esp_timer.h — host 桩：esp_timer_get_time 用 CLOCK_MONOTONIC。 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);

typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    const char *name;
    bool skip_unhandled_events;
    bool dispatch_method;
} esp_timer_args_t;

#define ESP_TIMER_TASK 0

int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t delay_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);

#ifdef __cplusplus
}
#endif
