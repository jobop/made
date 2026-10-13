# UI 模拟器（sim/）

在 Mac 上直接运行固件的真实 UI 代码（LVGL 9.6 + SDL2 窗口），**无需编译/烧录固件**。
WiFi / NVS / 音频 / USB 等硬件层用 `shims/` + `stubs/` 替身，UI 源码零改动。

## 用法

```sh
npm run sim            # 编译（增量）并打开模拟器窗口（鼠标=触摸）
npm run sim:shot       # 同上，OUT=out.bmp 截图模式
```

截图与手势（供 AI 自查布局，无需人交互）：

```sh
sim/build/sim --shot out.bmp                                  # 首页稳定后截图
sim/build/sim --shot out.bmp --swipe 120 300 120 40 --wait 30 # 上滑开抽屉再截
sim/build/sim --shot out.bmp --tap 40 290 --wait 20           # 点设置按钮再截
```

桩状态用环境变量切换：

| 变量 | 效果 |
| --- | --- |
| `MADE_SIM_PAIRING=paired` | 模拟已配对（主页 + 连接错误态；HTTP 桩永远失败） |
| `MADE_SIM_PAIRING=discovering`（默认） | 模拟未配对（配对抽屉布局） |
| `MADE_SIM_WIFI=1` | 模拟 WiFi 已连接（设置页可渲染扫描列表假数据） |

## 目录

- `lv_conf.h` — 模拟器 LVGL 配置（镜像固件 sdkconfig：RGB565 / montserrat 8..26 / 私有 API / qrcode / snapshot）
- `shims/` — ESP-IDF 头文件桩（esp_log/nvs/freertos/wifi/http…）与 brookesia `base::App`/`phone::App` 轻量替身
- `shims/lv_compat.h` — host clang 兼容垫片（设备工具链允许 `void*` 隐式转换，固件源码不动）
- `stubs/` — 上述桩的实现（`esp_stubs.cpp`）+ 非 UI 组件替身（`components_stubs.cpp`）
- `sim_main.cpp` — SDL 窗口、手势/点击注入、RGB565→BMP 截图

## 真实编译的源码

`esp_brookesia_app_vibe.cpp`（全部页面 UI）、`made_sprite.cpp`、`vibe_touch_keyboard`、
`vibe_i18n`、`vibe_theme`、内置豆素材 `img_sprite_bean.c`、字体 `font_puhui_16_4`、cJSON，
以及整个 `managed_components/lvgl__lvgl`（9.6.0）。

编译参数与固件 build-s3 对齐：`-std=gnu++2b -fpermissive`。

## 依赖

clang++（Xcode CLT）、`brew install sdl2 libpng`（本机已装）。

## 已知桩差异

- HTTP 请求永远失败（`esp_http_client_perform` 返回 `ESP_FAIL`）→ 主页显示「连接电脑」错误态；
  主题列表/会话/任务为空态。要测真实数据流，需给桩加本地 mock 桥接器（TODO）。
- 素材缓存 `made_sprite_cache` 恒为未命中 → 永远显示内置绿豆，不下发外部素材。
- NVS 是进程内内存，重启模拟器后设置不保留。
