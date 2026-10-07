# 刷机工具

一条命令完成认板、选目标、烧录。入口在仓库根：

```sh
npm run flash
```

实现是 `tools/flash.mjs`，纯 Node，无第三方依赖。它只是把 AGENTS.md 里那套手动烧录流程固化成脚本：先停桥接器放串口，再 `source export.sh`，再 `esptool write_flash`。工具把这些串起来，并加了认板、芯片校验和 NVS 保护。

## 目标

四类目标映射到三类固件。方板、圆板、码得侧按现状是同一套 `made/` 固件，做成别名。

| 目标 | 别名 | 芯片 | Flash | 镜像来源 |
| --- | --- | --- | --- | --- |
| `made` | 码得、方板、圆板、码得侧、方屏、圆屏、host | ESP32-S3 | 16MB | `made/build-s3`（读 `flasher_args.json`） |
| `receiver-s3` | receiver、接收器、接收器s3、rx-s3 | ESP32-S3 | 2MB | `receiver/release`（固定地址表） |
| `receiver-c3` | 接收器c3、c3、rx-c3、c3-uart | ESP32-C3 | 4MB | `receiver/release-esp32c3`（固定地址表） |
| `receiver-c3-usb` | 接收器c3usb、c3usb、c3-usb | ESP32-C3 | 4MB | `receiver/build-c3-usb`（读 `flasher_args.json`） |

`receiver-c3` 与 `receiver-c3-usb` 是同一块芯片的两套固件，区别在传输方式：前者走 UART（WCH 转串口板，`sdkconfig.c3uart`），后者走原生 USB（`sdkconfig.c3usb`）。**刷错变体桥接器收不到数据**，认板会按 USB 模式自动区分。

## 用法

```sh
npm run flash                              # 交互式：扫描 → 认板 → 推荐目标 → 确认 → 烧录
npm run flash -- --scan                    # 只扫描串口并认板，不烧
npm run flash -- --list                    # 列出全部目标
npm run flash -- made                      # 直接烧码得
npm run flash -- made -p /dev/cu.usbmodem101 -y
npm run flash -- receiver-c3-usb --build   # 先 idf.py build 再烧
npm run flash -- receiver-c3 --verify      # 烧前校验 SHA256SUMS
npm run flash -- made --dry-run            # 只打印将执行的命令
```

选项：

| 选项 | 说明 |
| --- | --- |
| `-p, --port <设备>` | 指定串口，默认自动扫描 |
| `--build` | 先构建（内部已处理 IDF 环境与 PYTHONPATH shim），再从构建目录取镜像 |
| `--scan` | 只扫描并认板，不烧录 |
| `--list` | 列出目标 |
| `--verify` | 烧前校验 release 目录的 `SHA256SUMS` |
| `--erase` | 整片擦除后再烧，会清 NVS，需手打 `erase` 二次确认 |
| `-y, --yes` | 跳过写入确认 |
| `--dry-run` | 只显示将执行的命令，不实际写入 |
| `--force` | 芯片型号不匹配时仍继续（危险） |
| `-h, --help` | 帮助 |

## 认板与推荐

烧前先对每个串口跑 `esptool chip_id` 和 `flash_id`，按下面规则推荐目标：

| 读到的信息 | 推荐目标 |
| --- | --- |
| ESP32-S3 且 PSRAM ≥ 8MB | `made`（码得） |
| ESP32-S3 且 PSRAM < 8MB | `receiver-s3` |
| ESP32-C3 且 `USB mode: USB-Serial/JTAG` | `receiver-c3-usb` |
| ESP32-C3 且无 USB 模式行 | `receiver-c3` |

S3 上码得与 S3 接收器靠 PSRAM 容量区分；容量读不到时，再看 Flash（码得 16MB、接收器 2MB）。两块板都枚举成 Espressif `303a:1001`，端口名不能用来认板。

## 关键设计

- **认板再烧**：先读芯片型号、PSRAM 容量和 USB 模式，再推荐并校验目标，不靠端口名猜。
- **芯片拦截**：目标芯片与实际不符直接拒绝并退出，跨型号必须显式 `--force`——防止接收器固件误刷进码得主机。
- **地址表真源**：`made` 与 `receiver-c3-usb` 直接读构建目录的 `flasher_args.json`，不写死地址，重新构建后自动同步；预构建的 `release` 目录用固定地址表。
- **NVS 保护**：默认不整片擦除，保住 NVS 里的 Wi-Fi、配对令牌与接收端热点密码。`--erase` 需二次确认。
- **构建联动**：`--build` 内部已封装两个已知坑——IDF 用 py3.9 环境（先 `export PATH="/usr/bin:$PATH"` 再 source），以及清掉导致 `idf.py` 崩的 `PYTHONPATH` shim（`unset PYTHONPATH`）。
- **非交互保护**：stdin 不是终端时不会卡在提问上，多串口会提示用 `-p` 指定。

## 写盘参数

统一用 esptool 4.12 的下划线写法，波特率 460800，`--before default_reset --after hard_reset`。

码得（地址来自 `made/build-s3/flasher_args.json`）：

| 地址 | 镜像 |
| --- | --- |
| 0x0 | bootloader |
| 0x8000 | 分区表 |
| 0xd000 | ota_data_initial |
| 0x20000 | 01_factory.bin |
| 0xac0000 | assets.bin |
| 0xec0000 | srmodels.bin |

接收器（S3 与 C3 相同）：

| 地址 | 镜像 |
| --- | --- |
| 0x0 | bootloader |
| 0x8000 | 分区表 |
| 0x10000 | vibe_receiver.bin |

分区没变、只改了应用时，码得可以只写 `0x20000`；界面缺字体或语音模型时再补 `assets.bin` 与 `srmodels.bin`——只写应用不写 assets，屏幕字体会缺。

## 烧录后

板子已硬复位。桥接器在运行会自动重新握手，否则在仓库根 `npm start`。接收器正常时会持续看到 `hello: mode=receiver`。码得锁屏不连桥接器、不上报心跳，大盘短暂显示离线是正常的，上滑解锁后才会重新上线。

## 排错

| 现象 | 处理 |
| --- | --- |
| 串口打不开 / resource busy | 桥接器正占着串口，先停掉它（工具会打印占用进程与 pid） |
| 串口没响应 | 按住 BOOT 再插一次 USB，重试 |
| 找不到 esptool | 装 ESP-IDF 工具链，或把 esptool 放进 PATH |
| 找不到镜像目录 | 先构建：`npm run flash -- <目标> --build` |
| 芯片不匹配 | 确认目标选对；跨型号确有需要才加 `--force` |
| 提示不是交互终端 | 把目标与参数一次给全，如 `npm run flash -- made -p <端口> -y` |

## 与手动烧录的关系

AGENTS.md「启动与烧录」一节的手动命令仍然有效，工具只是把它们自动化。需要精细控制（例如只写应用分区、或换自定义 `sdkconfig`）时按 AGENTS.md 手动执行。方板目前没有独立的固件工程，全仓库只有圆屏 1.85B 的 BSP；等方屏硬件到位，把它的镜像目录接到 `TARGETS.made` 同级即可，认板逻辑不用改。
