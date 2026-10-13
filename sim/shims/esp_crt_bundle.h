/* sim/shims/esp_crt_bundle.h — host 桩。 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_crt_bundle_attach(void *conf);

#ifdef __cplusplus
}
#endif
