#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NO_MEM = 0x101;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
constexpr esp_err_t ESP_ERR_INVALID_SIZE = 0x104;
constexpr esp_err_t ESP_ERR_TIMEOUT = 0x107;
constexpr esp_err_t ESP_ERR_INVALID_RESPONSE = 0x108;
using TickType_t = uint32_t;
using SemaphoreHandle_t = void*;
using TaskHandle_t = void*;
constexpr int pdTRUE = 1;
constexpr int pdPASS = 1;
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
inline void portENTER_CRITICAL(portMUX_TYPE* lock) { lock->lock(); }
inline void portEXIT_CRITICAL(portMUX_TYPE* lock) { lock->unlock(); }
constexpr TickType_t pdMS_TO_TICKS(uint32_t milliseconds) { return milliseconds; }
struct usb_serial_jtag_driver_config_t { size_t rx_buffer_size; size_t tx_buffer_size; };
constexpr int ESP_MAC_WIFI_STA = 0;
int64_t esp_timer_get_time();
esp_err_t esp_read_mac(uint8_t*, int);
esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t*);
void usb_serial_jtag_driver_uninstall();
bool usb_serial_jtag_is_connected();
int usb_serial_jtag_read_bytes(void*, size_t, TickType_t);
int usb_serial_jtag_write_bytes(const void*, size_t, TickType_t);
SemaphoreHandle_t xSemaphoreCreateMutex();
void vSemaphoreDelete(SemaphoreHandle_t);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
int xTaskCreate(void(*)(void*), const char*, uint32_t, void*, int, TaskHandle_t*);
void vTaskDelay(TickType_t);
void xTaskNotifyGive(TaskHandle_t);
uint32_t ulTaskNotifyTake(int, TickType_t);
int mbedtls_base64_encode(unsigned char*, size_t, size_t*, const unsigned char*, size_t);
int mbedtls_base64_decode(unsigned char*, size_t, size_t*, const unsigned char*, size_t);
