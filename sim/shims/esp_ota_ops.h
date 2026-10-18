/* sim/shims/esp_ota_ops.h — host 桩。模拟器没有更新分区。 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_SIZE_UNKNOWN 0xffffffffu

typedef uint32_t esp_ota_handle_t;

typedef struct esp_partition_t {
    char label[17];
} esp_partition_t;

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start);
esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size, esp_ota_handle_t *out_handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition);

#ifdef __cplusplus
}
#endif
