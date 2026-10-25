/* sim/shims/esp_random.h — host 桩：硬件随机数。 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void esp_fill_random(void *buf, size_t len);

#ifdef __cplusplus
}
#endif
