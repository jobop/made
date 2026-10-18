#pragma once

namespace vibe_motion {

// 摇晃检测回调（在检测任务上下文触发，别做重活，转投队列即可）。
using ShakeCallback = void (*)(void *ctx);

// 启动 IMU（QMI8658）摇晃检测。幂等；设备无 IMU 或初始化失败时静默不启动
// （只留一条错误日志），绝不拖垮主流程。
void start(ShakeCallback callback, void *ctx);

}
