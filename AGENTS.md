# AGENTS.md

## 连接方式全覆盖规则

**任何新功能、任何接口，实现时都必须考虑对以下全部接入方式可用：**

1. **USB 线直连**（无局域网，请求经 USB 串口帧转发）
2. **局域网 Wi-Fi 直连**（设备与电脑同网，HTTP 到电脑 8788）
3. **接收器转发**（码得连接收器热点，经 USB 串口转发到电脑）
4. **外网公网地址**（配对到公网 HTTP/HTTPS 端点，注意 HTTPS 证书与设备时间门）

具体要求：

- 新增设备侧 API 时，同时考虑三种传输：网络 HTTP（esp_http_client，HTTPS 挂
  `esp_crt_bundle` 并检查 `tls_time_ready`）、USB 串口帧（`vibe_usb::request`，
  响应上限 256KB）、接收器/USB 帧转发（桥接器 `usb-receiver.mjs` 的转发校验链）
- 桥接器新增端点时，明确注册在哪个服务（8788 设备 API / 8787 工作台），
  并确认各接入方式到达的端口一致
- 功能不可用于某种接入方式时，必须在 UI 上明确提示原因，而不是静默失败

各平台（macOS / Linux / Windows）上每种接入方式的可用性、限制依据与替代做法见 [平台支持](docs/PLATFORMS.md)。在目标机器上执行 `npm run doctor` 可直接得到本机结论（有阻断项时退出码为 1）。

## 市场能力划分

三种市场各管一层，并且只做包管理，与码得固件和桥接器运行时解耦：包要安装进桥接器才生效。插件市场控制码得本身（主题、管控、agent 运行时）；技能不在本仓库自建广场，沿用现成技能广场，挂到 agent 上扩展能力；专家市场只注入 agent 的人格。归类、边界和禁止混放的规则见 [市场能力划分](docs/MARKETS.md)。新增市场、插件、技能或专家时按该文档归类。桥接器里已有的插件、主题和示例目录是运行位置，不要拆进市场目录。

### 装包与发版只走市场命令行工具（硬规则）

往 `~/made/{plugins,themes}` 装包、把包发到市场，一律用市场对外发布的命令行工具，不要绕开它：

```sh
npx made-market install <包 ID>          # 装包（已存在加 --force 覆盖）
npx made-market validate <包目录>        # 发版前本地校验，与市场服务端同一套规则
npx made-market publish <包目录>         # 上架（需先 login）
npx made-market info <包 ID>             # 查看版本、校验和与安装位置
```

理由：这条命令自带 sha256 校验、实时索引与版本登记；手抄文件、直接跑仓库里的 CLI 脚本、或手工传归档，会绕过校验与登记，装出来的东西和市场里的不是同一个东西。

两条容易踩的坑：

- `install` 会**整目录重建**目标目录（`rmSync` 后按归档写入），用户已有的 `settings.json` 会被包内默认值覆盖，密钥和端点配置要先备份再装、装完恢复。
- `install` 从市场实时索引取包，不是从本地源码目录；改完插件源码要先 `publish` 出新版本，再 `install` 才会拿到新代码。判断装的是哪一版看 sha256，不看文件时间。

## 插件开放能力清单

管控插件的能力面 = 两条供给线的并集。写插件、评需求前先查这张表；市场网站有同步的[开放能力说明页](https://made-market.link/capabilities.html)，脚手架 `npx made-market init --kind control` 会把同一份清单生成进包里（`capabilities.json` + `contracts.d.ts`）。**给插件加新能力时，三处必须同步更新：本清单、市场 CLI 脚手架（made-market 仓库 `cli/`）、市场网站说明页（`site/capabilities.html`）。**

**板侧线 —— 清单由固件上报（`esp_brookesia_app_vibe.cpp` 的 `announceBoardCatalog`），扩清单要刷固件：**

| 事件（cadence 均为 edge） | 触发 |
| --- | --- |
| `boot.click` | 按键短按（语音意图） |
| `boot.double` | 双击（确认） |
| `boot.triple` | 三击（取消） |
| `boot.long` | 长按（回主页） |

| 命令 | 作用 |
| --- | --- |
| `caption.show` | 屏幕字幕（字段 `text`，另有时长等，见固件解析） |
| `audio.play` | 播放音频，可附带 WAV（16-bit PCM，8k–48kHz，≤256KB） |
| `theme.apply` | 应用主题插件 |
| `volume.set` | 音量 |
| `agent.select` | 切换当前助手 |
| `session.create` / `session.select` / `session.delete` | 会话管理 |
| `voice.start` | 主动点亮录音 |
| `task.confirm` / `task.cancel` | 审批待确认任务 |

**桥接器线 —— `bridge/src/plugins/enhance.mjs` 露出的句柄，改完重启桥接器即生效：**

- 触发面：`onEvent` / `onConnect` / `onDisconnect`（板侧事件与连接）、`onJob`（任务生命周期：`created` / `confirmed` / `started` / `progress` / `completed` / `handed_off` / `failed` / `cancelled`；插件自建任务不广播 `created`，防自激）、`onTurnInput` / `onTurnOutput`（回合管道：按注册顺序成链，返回字符串即 override，最后一个说话的算数；1500ms 超时，单个插件失败沿用上一环，绝不阻断任务；`onTurnInput` 改的是执行副本，任务原文不动）。
- `bridge` 门面（事件、钩子、通知的上下文里都拿得到）：`agents()` / `agent(id)` / `jobs()` / `job(id)` / `sessions(agentId?)` / `createTask({instruction, agentId?, projectId?, title?})`（自动建会话并落待确认任务，限流 10 次/分钟）/ `submitTurn({instruction, sessionId})` / `confirmJob(id)` / `cancelJob(id)` / `themes()` / `devices()` / `send(deviceId, command, audio?)` / `pending(deviceId)`。

接口类型真源是 `bridge/src/plugins/contracts.d.ts`，用法与完整示例在 `docs/PLUGINS.md`。能力归线的判断标准：板子上有没有这个命令/事件（固件的事），还是桥接器内存里有没有这个句柄（桥接器的事）——前者刷固件，后者改桥接器，插件永远只是清单的消费者。

## 启动与烧录

目录是 `made/`（码得固件）、`receiver/`（接收器固件）、`bridge/`（桥接器）。不要再从仓库根执行 `node src/server.mjs`，也不要增量编译仍指向 `firmware-brookesia` 或 `receiver-firmware` 的旧构建目录。

### 刷机工具

日常烧录用刷机工具 `tools/flash.mjs`，一条命令认板、选目标、烧录，默认不擦 Flash 以保住 NVS。它把下面那套手动流程固化了：

```sh
# 先停掉占着串口的桥接器，再刷
npm run flash                              # 交互式：扫描 → 认板 → 推荐目标 → 确认
npm run flash -- --scan                    # 只认板
npm run flash -- made -p /dev/cu.usbmodem101 -y
npm run flash -- receiver-c3-usb --build   # 先构建再烧
```

目标、认板与推荐规则、全部选项、写盘地址表和排错，见 [刷机工具](docs/FLASH.md)。以下是手动流程，需要精细控制（只写应用分区、换自定义 `sdkconfig`）时用。

### 启动桥接器

烧录或重新启动前，先停掉已经占用 `8787`、`8788` 和 `/dev/cu.usbmodem*` 的旧进程。改目录之前留下的桥接器会一直占着串口，esptool 会因此连不上板子。

在仓库根目录执行：

```sh
npm start
```

这会转到 `bridge/` 里的 `node src/server.mjs`。配置文件是 `bridge/config.local.json`，没有就从 `bridge/config.example.json` 复制。工作台是 `http://127.0.0.1:8787`，设备 API 是 `8788`，局域网发现是 UDP `8789`。测试也在仓库根执行 `npm test`。

#### 启动环境：别从沙箱会话里拉起桥接器

WorkBuddy / 沙箱会话的 shell 会往环境里塞**会话级的本地代理**
（`HTTP_PROXY=HTTPS_PROXY=http://127.0.0.1:<随机端口>`）和一堆 shim 变量，而桥接器用
`spawn(..., { env: process.env })` 把它们**原样转发给子 CLI**。codex 是 Rust reqwest，认
`HTTPS_PROXY`：请求全发给一个早已消失的端口后会**无限重试**（进程活着、0% CPU、
一个 socket 都没有），任务永远停在「正在分析任务」。Node 写的助手不认这个变量，所以自身
没事，但会被卡住的那条车道堵在后面——看起来像「所有助手都卡」。

从会话里启动桥接器时，必须先剔除 `HTTP_PROXY`/`HTTPS_PROXY`/`ALL_PROXY`/`NO_PROXY`
（大小写都要）、`NODE_OPTIONS`、`PYTHONPATH`、`BASH_ENV` 和 `CODEBUDDY_*`。
排查口诀：`ps eww -p <桥接器 PID>` 看它环境里有没有代理；`lsof -a -p <CLI PID> -i`
为空就说明不是网络在等。

#### 任务车道

`bridge/src/jobs.mjs` 的 `pump()` 按**会话**分车道：会话之间可以并行，同一会话恒为串行
（编程工具按会话线程加写锁，同会话并行必然冲突，`submit()` 也会直接拒绝同一会话的
第二条并发指令）。同时执行的会话数默认 2，用 `bridge/config.local.json` 的
`maxParallelJobs` 或环境变量 `VIBE_MAX_PARALLEL` 调整，取值 1–4。

码得锁屏不连接桥接器、不上报心跳。烧录重启后大盘显示离线是预期现象，上滑解锁后才会重新上线。

### 认板再烧

两块板都可以枚举成 Espressif `303a:1001`。先对每个口执行 `python -m esptool --port /dev/cu.usbmodemXXXX chip_id`，再决定镜像。端口会随 USB 口变化。

- ESP32-S3 且带 8MB PSRAM：码得（微雪 1.85B），16MB Flash，只用 `made/` 的镜像。
- ESP32-C3：接收器，4MB Flash，用 `receiver/` 的镜像。C3 有两套固件变体，认板看 esptool 的 `USB mode` 行：报 `USB-Serial/JTAG` 是原生 USB（`sdkconfig.c3usb`），否则是 UART / WCH 转串口（`sdkconfig.c3uart`）。刷错变体桥接器收不到数据。

这些规则已由 [刷机工具](docs/FLASH.md) 自动执行，手动烧录时按上面的判据认板。

2026-10-06 这次实板是：码得 `/dev/cu.usbmodem1101`（MAC `84:c7:bb:78:e4:30`），接收器 `/dev/cu.usbmodem1301`（MAC `10:20:ba:ce:f4:1c`，原生 USB，不是 WCH `1a86:55d3`）。

本机 ESP-IDF 5.5 自带的 esptool 是 4.12，参数用下划线：`--before default_reset --after hard_reset write_flash`。先 `source "$HOME/esp/esp-idf/export.sh"`，再用它的 `python -m esptool`。`receiver/release/FLASH.md` 和 `receiver/release-esp32c3/FLASH.md` 里的 `default-reset`、`write-flash` 是更新 esptool 的写法，在 4.12 上会直接失败。不要整片擦除 Flash，否则 NVS 里的 Wi-Fi、配对和接收器热点密码会丢。

### 烧录码得

`made/build` 和 `made/build-c3usb` 的 CMake 缓存仍写着改目录前的 `firmware-brookesia`。`build-c3usb` 只是历史目录名，目标芯片是 esp32s3。不要在这两个目录上增量编译。`made/release/` 没有可烧录镜像。在 `made/` 下新建构建目录：

```sh
source "$HOME/esp/esp-idf/export.sh"
cd made
idf.py -B build-s3 build
idf.py -B build-s3 -p /dev/cu.usbmodemXXXX flash
```

`idf.py flash` 按该目录的 `flasher_args.json` 写入 bootloader（`0x0`）、分区表（`0x8000`）、factory 应用 `01_factory.bin`（`0x20000`）、`ota_data_initial.bin`（`0xd000`）、`assets.bin`（`0xac0000`）和 `srmodels.bin`（`0xec0000`）。分区没变、只改了应用时，可以只写 `0x20000`；界面缺字体或语音模型时再补 assets 和 srmodels。只写应用、不写 assets，屏幕字体会缺。

### 烧录接收器

当前这块 C3 走原生 USB。`receiver/build-c3usb` 的 CMake 缓存仍写着 `receiver-firmware`，不要增量编译它。`receiver/release/` 是 ESP32-S3 接收器镜像，不能刷到这块 C3 上，也不能刷到码得主机上。新建构建目录：

```sh
source "$HOME/esp/esp-idf/export.sh"
cd receiver
idf.py -B build-c3usb -D SDKCONFIG=sdkconfig.c3usb \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c3;sdkconfig.defaults.esp32c3usb" \
  -D IDF_TARGET=esp32c3 build
idf.py -B build-c3usb -D SDKCONFIG=sdkconfig.c3usb -p /dev/cu.usbmodemXXXX flash
```

已有这个构建目录里的镜像时，用 esptool 4.12 写入并核对哈希：

```sh
python -m esptool --chip esp32c3 -p /dev/cu.usbmodemXXXX -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0 bootloader/bootloader.bin \
  0x8000 partition_table/partition-table.bin \
  0x10000 vibe_receiver.bin
```

烧录后重新 `npm start`。大盘「USB 连接」应显示接收端已连接、热点名，以及 `http://192.168.4.1:8788`。两块板都插着时，桥接器会打开每一个 `303a:1001`；码得自己的启动日志出现在桥接器输出里，不代表接收器握手失败。接收器正常时会持续看到 `hello: mode=receiver`。码得上滑解锁后，在齿轮 → 连接 → 接收端里选用大盘上的热点，再核对六位码。

## 外壳模型（3d-model）

码得外壳不是手工建模，全部由 `3d-model/make_case.py` 参数化生成，每个部位在脚本里都有对象名和变量。**改外壳前先查部位词典：[外壳部位词典](3d-model/PARTS.md)**（前框 front、后壳 back、柱塞按 MIC 端 / USB 端定向命名）。

两份索引：

- **`3d-model/PARTS.md`** — 部位 → 脚本对象/变量对照表（文字版）。
- **`3d-model/PARTS-diagram.svg`** — 部位标注图（后壳俯视，部位名 + 尺寸 + 变量名画在图上）。
  **触发词：「3d 外壳标注图」**。用户一说这个词，直接打开这张 SVG 给他看，不要用文字复述代替。

规则：

- 提改动、写注释、开分支，一律用词典里的部位名，不要用“左边/上面/中间那块”这类说法。
- 方位按词典的坐标系：`x=0` 是 **MIC 端**，`x=OUT_L` 是 **USB 端**；前壳 `z=0` 是外面板，后壳 `z=0` 是外背面。
- 改动落在 `make_case.py` 的变量上，改完重跑脚本导出三个 STL（前框、后壳、柱塞）并重渲 `preview.png`，不要直接编辑 STL / 3MF 产物。
- `boss_top`、`seat_z` 与柱塞长度是联动链，改按键柱高度要连带改柱塞，具体见词典。
- 部位位置或尺寸变了，同步更新 `PARTS-diagram.svg`（图是按脚本参数手绘的，不同步就会漂移）。
