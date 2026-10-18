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
#include <initializer_list>
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
    // 地址两种都试（模块 ADDR 引脚接法不同 → 0x6B/0x6A），失败后周期重试：
    // 实测首次 init 可能撞上 IMU 上电时序（WHO_AM_I 读失败 0x103），
    // 开机即退永不重试会让摇一摇永久失效。
    qmi8658_dev_t dev = {};
    bool ready = false;
    bool scanned = false;
    while (!ready) {
        for (const int address : {QMI8658_ADDRESS_HIGH, QMI8658_ADDRESS_LOW}) {
            esp_err_t err = bsp_i2c_init();
            if (err != ESP_OK) { ESP_LOGE(TAG, "[motion] i2c init failed 0x%x", err); break; }
            i2c_master_bus_handle_t bus = nullptr;
            err = i2c_master_get_bus_handle(BSP_I2C_NUM, &bus);
            if (err != ESP_OK || bus == nullptr) { ESP_LOGE(TAG, "[motion] i2c bus handle missing 0x%x", err); break; }
            if (!scanned) {
                // 一次性总线普查：确认这条 I2C 上到底挂着哪些设备（触摸应答，
                // IMU 是否在位一目了然），排查完可删。
                char found[64] = {0};
                size_t used = 0;
                for (int probe = 1; probe < 120 && used < sizeof(found) - 8; ++probe) {
                    if (i2c_master_probe(bus, probe, 50) == ESP_OK) {
                        used += snprintf(found + used, sizeof(found) - used, " 0x%02x", probe);
                    }
                }
                ESP_LOGW(TAG, "[motion] i2c scan:%s", found);
                scanned = true;
            }
            err = qmi8658_init(&dev, bus, address);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "[motion] qmi8658 init failed addr=0x%02x err=0x%x", address, err);
                continue;
            }
            qmi8658_set_accel_unit_mg(&dev, true);
            qmi8658_set_accel_range(&dev, QMI8658_ACCEL_RANGE_8G);
            qmi8658_set_accel_odr(&dev, QMI8658_ACCEL_ODR_500HZ);
            qmi8658_enable_accel(&dev, true);
            ESP_LOGI(TAG, "[motion] imu ready addr=0x%02x", address);
            ready = true;
            break;
        }
        if (!ready) vTaskDelay(pdMS_TO_TICKS(15000));   // IMU 缺席/未上电：安静重试
    }
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
