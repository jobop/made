// 摇晃感知（行动系统的事件源之一）：加速度计独立任务采样，
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

// LIS3DH / SC7A20 兼容寄存器集（板卡实际焊的是这类，而非 BSP 标称的
// QMI8658——i2c 普查实锤：0x18 稳定应答，0x6A/0x6B 无设备）。
constexpr uint8_t kLisWhoAmIReg = 0x0F;
constexpr uint8_t kLisCtrlReg1 = 0x20;        // ODR + XYZ enable
constexpr uint8_t kLisCtrlReg4 = 0x23;        // BDU / FS 量程 / HR
constexpr uint8_t kLisOutXLow = 0x28 | 0x80;  // |0x80 = 寄存器地址自增
constexpr uint8_t kLisWhoAmIVal = 0x33;       // LIS3DH/LIS2DH12/SC7A20 同值

bool lis3dhInit(i2c_master_bus_handle_t bus, uint8_t addr, i2c_master_dev_handle_t *out)
{
    // C++ 编译单元不吃嵌套指定初始化器（qmi8658.c 是 C 才行），逐字段赋值。
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = addr;
    cfg.scl_speed_hz = 400000;
    cfg.flags.disable_ack_check = false;
    if (i2c_master_bus_add_device(bus, &cfg, out) != ESP_OK || !*out) return false;
    uint8_t who = 0;
    if (i2c_master_transmit_receive(*out, &kLisWhoAmIReg, 1, &who, 1, 100) != ESP_OK ||
        who != kLisWhoAmIVal) {
        // 非 LIS3DH 系：dump 前 16 个寄存器（Bosch/STK 系 WAI 在 0x00、
        // ST 系在 0x0F、国产芯片各有位置），指纹定位真身。
        uint8_t reg0 = 0x00, dump[16] = {0};
        (void)i2c_master_transmit_receive(*out, &reg0, 1, dump, sizeof(dump), 200);
        char hex[80] = {0};
        size_t used = 0;
        for (int i = 0; i < 16 && used < sizeof(hex) - 6; ++i) {
            used += snprintf(hex + used, sizeof(hex) - used, " %02x", dump[i]);
        }
        ESP_LOGW(TAG, "[motion] imu dump addr=0x%02x who0f=0x%02x:%s", addr, who, hex);
        i2c_master_bus_rm_device(*out);
        *out = nullptr;
        return false;
    }
    // 400Hz + XYZ 使能；BDU=1、FS=10(±8g)、HR=1——摇一摇大甩动不削顶。
    const uint8_t ctrl1[2] = {kLisCtrlReg1, 0x77};
    const uint8_t ctrl4[2] = {kLisCtrlReg4, 0xA8};
    if (i2c_master_transmit(*out, ctrl1, 2, 100) != ESP_OK ||
        i2c_master_transmit(*out, ctrl4, 2, 100) != ESP_OK) {
        ESP_LOGW(TAG, "[motion] lis3dh config failed addr=0x%02x", addr);
        i2c_master_bus_rm_device(*out);
        *out = nullptr;
        return false;
    }
    return true;
}

// 读三轴，输出 mg。±8g HR 模式 1 LSB ≈ 3.9 mg（12bit 左对齐，先除 16）。
bool lis3dhReadMg(i2c_master_dev_handle_t dev, float *x, float *y, float *z)
{
    uint8_t raw[6] = {0};
    if (i2c_master_transmit_receive(dev, &kLisOutXLow, 1, raw, 6, 100) != ESP_OK) return false;
    const auto axis = [](const uint8_t *p) -> float {
        const int16_t v = static_cast<int16_t>(static_cast<uint16_t>(p[0]) |
                                               (static_cast<uint16_t>(p[1]) << 8)) >> 4;
        return static_cast<float>(v) * 3.9f;
    };
    *x = axis(raw + 0);
    *y = axis(raw + 2);
    *z = axis(raw + 4);
    return true;
}

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
    // 驱动选择：优先 QMI8658（BSP 标称 BOM），失败后 probe LIS3DH 系
    // （0x18/0x19；本板实焊就是这类，QMI8658 地址 0x6A/0x6B 无设备）。
    // 失败后周期重试：实测首次 init 可能撞上 IMU 上电时序。
    enum class ImuKind { None, Qmi, Lis };
    ImuKind kind = ImuKind::None;
    bool first_scan = true;
    qmi8658_dev_t qmi_dev = {};
    i2c_master_dev_handle_t lis_dev = nullptr;
    while (kind == ImuKind::None) {
        i2c_master_bus_handle_t bus = nullptr;
        for (const int address : {QMI8658_ADDRESS_HIGH, QMI8658_ADDRESS_LOW}) {
            esp_err_t err = bsp_i2c_init();
            if (err != ESP_OK) { ESP_LOGE(TAG, "[motion] i2c init failed 0x%x", err); break; }
            err = i2c_master_get_bus_handle(BSP_I2C_NUM, &bus);
            if (err != ESP_OK || bus == nullptr) { ESP_LOGE(TAG, "[motion] i2c bus handle missing 0x%x", err); break; }
            // 首轮做全表普查（~6s，仅失败路径；结论 2026-10-20：0x18 是
            // ES8311 codec 不是 IMU，0x38 是触摸——本板无应答的加速度计，
            // 官方 1.85B 标称的 QMI8658@0x6B 未上电/损坏/未焊）。
            if (first_scan) {
                char found[64] = {0};
                size_t used = 0;
                for (int probe = 1; probe < 120 && used < sizeof(found) - 8; ++probe) {
                    if (i2c_master_probe(bus, probe, 50) == ESP_OK) {
                        used += snprintf(found + used, sizeof(found) - used, " 0x%02x", probe);
                    }
                }
                ESP_LOGW(TAG, "[motion] i2c scan:%s", found);
                first_scan = false;
            }
            err = qmi8658_init(&qmi_dev, bus, address);
            if (err == ESP_OK) {
                qmi8658_set_accel_unit_mg(&qmi_dev, true);
                qmi8658_set_accel_range(&qmi_dev, QMI8658_ACCEL_RANGE_8G);
                qmi8658_set_accel_odr(&qmi_dev, QMI8658_ACCEL_ODR_500HZ);
                qmi8658_enable_accel(&qmi_dev, true);
                ESP_LOGI(TAG, "[motion] imu ready qmi8658 addr=0x%02x", address);
                kind = ImuKind::Qmi;
                break;
            }
            ESP_LOGW(TAG, "[motion] qmi8658 init failed addr=0x%02x err=0x%x", address, err);
        }
        if (kind == ImuKind::None && bus != nullptr) {
            for (const uint8_t addr : {0x18u, 0x19u}) {
                if (lis3dhInit(bus, addr, &lis_dev)) {
                    ESP_LOGI(TAG, "[motion] imu ready lis3dh addr=0x%02x", addr);
                    kind = ImuKind::Lis;
                    break;
                }
            }
        }
        if (kind == ImuKind::None) vTaskDelay(pdMS_TO_TICKS(60000));   // IMU 缺席：安静慢重试
    }
    float baseline = 1000.0f;    // 静态基线（mg），EMA 慢速跟踪
    int kicks = 0;
    int64_t first_kick_us = 0;
    int64_t cooldown_until = 0;
    while (true) {
        vTaskDelay(kSamplePeriod);
        float x = 0, y = 0, z = 0;
        bool ok = false;
        if (kind == ImuKind::Lis) {
            ok = lis3dhReadMg(lis_dev, &x, &y, &z);
        } else {
            // 注意：qmi8658.h 声明了 qmi8658_read_accel_mg 但驱动没实现，用
            // read_accel（单位由 set_accel_unit_mg 决定，上面已设为 mg）。
            ok = qmi8658_read_accel(&qmi_dev, &x, &y, &z) == ESP_OK;
        }
        if (!ok) continue;
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
