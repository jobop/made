/* sim/shims/freertos/FreeRTOS.h — host 桩：任务用 std::thread，延时用 sleep。 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef size_t StackType_t;
typedef long eTaskState;

#define pdTRUE    1
#define pdFALSE   0
#define pdPASS    pdTRUE
#define pdFAIL    0
#define pdMS_TO_TICKS(ms)        ((TickType_t)(ms))
#define portTICK_PERIOD_MS       1u
#define portMAX_DELAY            0xFFFFFFFFu
#define portNUM_PROCESSORS       2
/* 自旋锁宿主桩：单进程内竞争窗口极小，临界区一律空操作（仅保护
 * 布尔/短 char 数组， risk 可忽略）。 */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux)      ((void)(mux))
#define portEXIT_CRITICAL(mux)       ((void)(mux))
#define tskIDLE_PRIORITY         0
#define configSTACK_DEPTH_TYPE   uint32_t

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef void *SemaphoreHandle_t;

typedef uint32_t eNotifyAction;
#define eNoAction                 0u
#define eSetBits                  1u
#define eIncrement                2u
#define eSetValueWithOverwrite    3u
#define eSetValueWithoutOverwrite 4u

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_depth,
                       void *arg, UBaseType_t priority, TaskHandle_t *handle);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack_depth,
                                   void *arg, UBaseType_t priority, TaskHandle_t *handle, BaseType_t core);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t handle);
TickType_t xTaskGetTickCount(void);
const char *pcTaskGetName(TaskHandle_t handle);
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action);
BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit, uint32_t *value, TickType_t wait);

#ifdef __cplusplus
}
#endif
