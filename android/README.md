# made 安卓端（Android target）

把 made 从「只跑在 ESP32-S3 硬件上」扩展成**第二个运行平台：安卓手机/平板**。
目标是让同一套 UI/应用层代码（`VibeCoding`）在安卓设备上直接跑起来，而不是另写一套 App。

## 为什么工作量不大

`sim/` 已经是「在非 ESP32 上跑 made UI」的完整范例：

- 显示/输入后端用的是 **SDL2**（`lv_sdl_window_create` + `lv_sdl_mouse_create`）。
- ESP-IDF 的 API（freertos / nvs / wifi / bsp / esp_log …）由 `sim/shims/` + `sim/stubs/` 桩掉。
- 应用层 `made/components/brookesia_app_vibe/*.cpp` **一行不改**，直接编译。

而 **SDL2 官方原生支持安卓**（SDL Activity + NDK native_main）。所以安卓端 =
把 sim 的 SDL2 后端从「桌面窗口」换成「安卓窗口」，其余（LVGL、shim、应用层）原样复用。

```
        ┌─────────────────────────────────────┐
        │  应用层  brookesia_app_vibe (VibeCoding)   │  ← ESP32 / sim / android 三者共用，零改动
        ├─────────────────────────────────────┤
        │  LVGL  +  made_layout 自适应布局          │  ← 共用
        ├──────────┬──────────┬─────────────────┤
        │ ESP-IDF  │ sim/SDL2 │ android/SDL2     │  ← 三个平台后端，各自实现
        │ (真硬件) │ (PC 窗口) │ (安卓手机屏幕)    │
        └──────────┴──────────┴─────────────────┘
```

## 平台边界（哪些共用 / 哪些要新写）

| 部分 | 来源 | 安卓要做的 |
|---|---|---|
| UI 应用层 `brookesia_app_vibe/*.cpp` | 仓库现有 | **复用，不改** |
| LVGL (`managed_components/lvgl__lvgl`) | 仓库现有 | 复用，CMake 编进 .so |
| ESP-IDF 桩 | `sim/shims/` + `sim/stubs/` | **直接复用**（已是 host 桩） |
| 屏幕尺寸自适应 | `made_layout.hpp` | 复用；安卓大屏自动缩放 |
| 显示后端 | sim 用 `lv_sdl_window_create` | 复用（SDL2-Android 自带） |
| 输入 | sim 用 `lv_sdl_mouse_create` | 复用（SDL2 鼠标事件 = 安卓触屏） |
| native 入口 | `sim/sim_main.cpp` | 新写精简版 `android_main.cpp` |
| Gradle/Manifest 壳 | 无 | 新建（SDL2 官方安卓模板） |

## 前置环境（本机当前全部缺失，需安装）

本机现在**没有**：JDK、Android SDK、NDK、CMake/Ninja。要真正出 APK 需先装：

- JDK 17（`brew install --cask temurin@17`）
- Android SDK + NDK（`sdkmanager` 装 `ndk;26.x`、`platforms;android-34`、`cmake`）
- SDL2 for Android（通过 Gradle 依赖 `org.libsdl:sdl2` 或官方预编译 AAR）

## 构建步骤（环境就绪后）

```bash
cd android
./gradlew assembleDebug          # 编出 app/build/outputs/apk/debug/app-debug.apk
adb install -r app-debug.apk      # 装到连机手机/模拟器
adb logcat | grep made            # 看日志
```

屏幕尺寸：安卓窗口默认全屏，`made_layout.hpp` 会按真实屏幕短边自动缩放，
无需像 sim 那样指定 SCREEN。

## 目录

```
android/
  README.md              ← 本文件
  app/
    build.gradle         ← 模块构建（待 Android Studio 同步）
    src/main/
      AndroidManifest.xml
      jni/
        CMakeLists.txt   ← 把 sim 的源文件清单翻译过来编进 .so
        android_main.cpp ← SDL2-Android 精简入口
```

> 状态：分支 `feat/android-target`。骨架已搭，待安装安卓工具链后首次构建验证。
