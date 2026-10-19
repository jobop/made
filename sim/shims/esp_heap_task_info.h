// sim 桩：esp_heap_task_info.h（真机 CONFIG_HEAP_TASK_TRACKING 才有实现，
// sim 只需要编译通过——logHeapDiagnostics 在 sim 里同样可调用，行为为空）。
#pragma once

#include <cstddef>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NUM_HEAP_TASK_CAPS 4

typedef struct {
    TaskHandle_t task;
    size_t size[NUM_HEAP_TASK_CAPS];
    size_t count[NUM_HEAP_TASK_CAPS];
} heap_task_totals_t;

typedef struct {
    TaskHandle_t task;
    void *address;
    uint32_t size;
} heap_task_block_t;

typedef struct {
    int32_t caps[NUM_HEAP_TASK_CAPS];
    int32_t mask[NUM_HEAP_TASK_CAPS];
    TaskHandle_t *tasks;
    size_t num_tasks;
    heap_task_totals_t *totals;
    size_t *num_totals;
    size_t max_totals;
    heap_task_block_t *blocks;
    size_t max_blocks;
} heap_task_info_params_t;

inline size_t heap_caps_get_per_task_info(heap_task_info_params_t *params) {
    if (params && params->num_totals) *params->num_totals = 0;
    return 0;
}
