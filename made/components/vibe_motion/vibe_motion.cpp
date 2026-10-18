// 摇晃感知（行动系统的事件源之一）：QMI8658 加速度计独立任务采样，
// 检测到「摇一摇」手势后触发回调（由上层转成 motion.shake 板侧事件）。
//
// 判定：静态时加速度模长 ≈ 1g（随姿态略有偏移），用慢速 EMA 跟踪基线；
// 模长偏离基线超过 kKickMg 记一记「甩动」，kWindowUs 窗口内凑满
// kKicksNeeded 记即判定摇晃，触发后进入冷却，防止连续刷事件。
#include "vibe_motion.hpp"

#include <cmath>
#include <atomic>
#include <cstdint>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "bsp/esp32_s3_touch_lcd_1_85B.h"
#include "qmi8658.h"

namespace vibe_motion {
namespace {

constexpr const char *TAG = "vibe_motion";

constexpr float kKickMg = 550.0f;         // 模长偏离基线阈值（mg）
constexpr int kKicksNeeded = 3;           // 窗口内需要几记甩动
constexpr int64_t kWindowUs = 700000;     // 甩动计数窗口
constexpr int64_t kCooldownUs = 2500000;  // 触发后冷却
constexpr float kBaselineAlpha = 0.02f;   // 基线 EMA 系数（慢速跟随姿态）
constexpr TickType_t kSamplePeriod = pdMS_TO_TICKS(20);

std::atomic<bool> started{false};

struct Hook {
    ShakeCallback callback;
    void *ctx;
};

void motionTask(void *param)
{
    Hook hook = *static_cast<Hook *>(param);
    delete static_cast<Hook *>(param);

    // 初始化：I2C 总线由 BSP 管理（幂等），拿句柄后手动初始化 IMU，
    // 全程错误检查不用 ESP_ERROR_CHECK——IMU 缺席不能拖垮整机。
    qmi8658_dev_t dev = {};
    do {
        esp_err_t err = bsp_i2c_init();
        if (err != ESP_OK) { ESP_LOGE(TAG, "[motion] i2c init failed 0x%x", err); break; }
        i2c_master_bus_handle_t bus = nullptr;
        err = i2c_master_get_bus_handle(BSP_I2C_NUM, &bus);
        if (err != ESP_OK || bus == nullptr) { ESP_LOGE(TAG, "[motion] i2c bus handle missing 0x%x", err); break; }
        err = qmi8658_init(&dev, bus, QMI8658_ADDRESS_HIGH);
        if (err != ESP_OK) { ESP_LOGE(TAG, "[motion] qmi8658 init failed 0x%x", err); break; }
        qmi8658_set_accel_unit_mg(&dev, true);
        qmi8658_set_accel_range(&dev, QMI8658_ACCEL_RANGE_8G);
        qmi8658_set_accel_odr(&dev, QMI8658_ACCEL_ODR_500HZ);
        qmi8658_enable_accel(&dev, true);
        ESP_LOGI(TAG, "[motion] imu ready");
        goto running;
    } while (false);
    vTaskDelete(nullptr);   // 初始化失败：本任务退出，不再重试
    return;

running:
    float baseline = 1000.0f;    // 静态基线（mg），EMA 慢速跟踪
    int kicks = 0;
    int64_t first_kick_us = 0;
    int64_t cooldown_until = 0;
    while (true) {
        vTaskDelay(kSamplePeriod);
        // 注意：qmi8658.h 声明了 qmi8658_read_accel_mg 但驱动没实现，用
        // read_accel（单位由 set_accel_unit_mg 决定，上面已设为 mg）。
        float x = 0, y = 0, z = 0;
        if (qmi8658_read_accel(&dev, &x, &y, &z) != ESP_OK) continue;
        const float magnitude = sqrtf(x * x + y * y + z * z);
        baseline += (magnitude - baseline) * kBaselineAlpha;
        const int64_t now = esp_timer_get_time();
        if (now < cooldown_until) continue;
        if (fabsf(magnitude - baseline) <= kKickMg) continue;
        if (kicks == 0 || now - first_kick_us > kWindowUs) {
            kicks = 1;
            first_kick_us = now;
        } else if (++kicks >= kKicksNeeded) {
            cooldown_until = now + kCooldownUs;
            kicks = 0;
            ESP_LOGI(TAG, "[motion] shake");
            hook.callback(hook.ctx);
        }
    }
}

}  // namespace

void start(ShakeCallback callback, void *ctx)
{
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;
    auto *hook = new Hook{callback, ctx};
    if (xTaskCreate(motionTask, "vibe_motion", 4096, hook, 3, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "[motion] task create failed");
        delete hook;
        started.store(false);
    }
}

}  // namespace vibe_motion
