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

## 市场能力划分

三种市场各管一层，并且只做包管理，与码得固件和桥接器运行时解耦：包要安装进桥接器才生效。插件市场控制码得本身（主题、管控、agent 运行时）；技能不在本仓库自建广场，沿用现成技能广场，挂到 agent 上扩展能力；专家市场只注入 agent 的人格。归类、边界和禁止混放的规则见 [市场能力划分](docs/MARKETS.md)。新增市场、插件、技能或专家时按该文档归类。桥接器里已有的插件、主题和示例目录是运行位置，不要拆进市场目录。

## 启动与烧录

目录是 `made/`（码得固件）、`receiver/`（接收器固件）、`bridge/`（桥接器）。不要再从仓库根执行 `node src/server.mjs`，也不要增量编译仍指向 `firmware-brookesia` 或 `receiver-firmware` 的旧构建目录。

### 启动桥接器

烧录或重新启动前，先停掉已经占用 `8787`、`8788` 和 `/dev/cu.usbmodem*` 的旧进程。改目录之前留下的桥接器会一直占着串口，esptool 会因此连不上板子。

在仓库根目录执行：

```sh
npm start
```

这会转到 `bridge/` 里的 `node src/server.mjs`。配置文件是 `bridge/config.local.json`，没有就从 `bridge/config.example.json` 复制。工作台是 `http://127.0.0.1:8787`，设备 API 是 `8788`，局域网发现是 UDP `8789`。测试也在仓库根执行 `npm test`。

码得锁屏不连接桥接器、不上报心跳。烧录重启后大盘显示离线是预期现象，上滑解锁后才会重新上线。

### 认板再烧

两块板都可以枚举成 Espressif `303a:1001`。先对每个口执行 `python -m esptool --port /dev/cu.usbmodemXXXX chip_id`，再决定镜像。端口会随 USB 口变化。

- ESP32-S3 且带 8MB PSRAM：码得（微雪 1.85B），16MB Flash，只用 `made/` 的镜像。
- ESP32-C3：接收器，4MB Flash，只用 `receiver/` 的镜像。

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
