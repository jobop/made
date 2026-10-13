/* sim/shims/esp_system.h — host 桩。 */
#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void esp_restart(void);
uint32_t esp_get_free_heap_size(void);
uint32_t esp_random(void);

#ifdef __cplusplus
}
#endif
