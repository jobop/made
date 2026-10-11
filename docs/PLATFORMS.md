# 平台支持

一句话：**macOS 完整，Linux 基本完整，Windows 三条网络链路可用、USB 要手动指定端口、刷机要 ESP-IDF 环境。**

不用靠猜——在目标机器上跑一次自检，它会按当前平台逐项报告结论：

```sh
npm run doctor          # = node bridge/src/server.mjs --doctor
```

自检检查运行环境、依赖命令（git / npm / esptool）、五种接入方式与已装插件；有阻断项时退出码为 1，可以放进脚本里。

## 能力矩阵

| 接入方式 | macOS | Linux | Windows |
| --- | --- | --- | --- |
| USB 线直连 | 可用 | 可用 | 受限：需手动指定端口 |
| 接收器转发（走同一条 USB 串口） | 可用 | 可用 | 受限：同上 |
| 局域网 Wi-Fi 直连 | 可用 | 可用 | 可用 |
| 外网公网地址 | 可用（需内网穿透工具） | 同左 | 同左 |
| 刷机 `npm run flash` | 可用 | 可用 | 可用（`--build` 需 IDF 环境） |

## 各条限制的依据与办法

### 串口自动发现只在 macOS / Linux

`bridge/src/usb-receiver.mjs` 的 `discoverUsbSerialPorts()`：macOS 走 `ioreg` 并按 USB VID/PID 匹配（`303a:1001` 码得、`1a86:55d3` 接收器），Linux 走 `/sys/class/tty` 读 `idVendor`/`idProduct`；**其他平台直接返回空数组**。

界面不静默失败：抓不到端口时显示「此系统需手动配置 VIBE_USB_PORT」（`usb-receiver.mjs:492`，文案在 `i18n.mjs`）。

Windows 上的办法——先在设备管理器里查到端口号，然后：

```sh
set VIBE_USB_PORT=COM3            :: CMD
$env:VIBE_USB_PORT='COM3'         # PowerShell
```

`COM10` 及以上写成 `\\.\COM10`。配置校验按平台放行这两种写法（`config.mjs` 的 `validateUsbPort`），相对路径与控制字符一律拒绝。

### 串口参数只在 macOS / Linux 设置

`configureSerial()` 用 `stty` 设 921600 8N1 raw，只在 macOS / Linux 生效；Windows 上没有 `stty`，这一步被跳过。后果分两种：

- 接收器的**原生 USB** 变体不依赖波特率，Windows 上仍可用；
- **WCH 转串口**变体依赖波特率设置，Windows 上大概率不通。这是已知缺口，不是配置问题。

### 占用检测只在 macOS / Linux

桥接器与刷机工具都用 `lsof` 判断串口被谁占用；Windows 上这一步为空，端口被占用时改由 esptool 自己报错（`Access is denied`）。**刷机前先停掉桥接器**，这条在三个平台上都成立。

### 刷机工具的 Windows 支持

- 端口名来自 PowerShell 的 `SerialPort.GetPortNames()`，拿不到再退回注册表 `HKLM\HARDWARE\DEVICEMAP\SERIALCOMM`；`COM10` 及以上自动转成 `\\.\COMNN`。
- `esptool` 按 PATH 找（`esptool.exe` / `esptool.py`），找不到再试各 Python 安装器的 `Scripts` 目录，最后试 `<python> -m esptool`。
- `--build` 需要在 ESP-IDF 安装器生成的「ESP-IDF PowerShell」或「ESP-IDF Command Prompt」里运行——Windows 上没有 `zsh`，也没有 IDF 的 `export.sh`。找不到 `idf.py` 时会直接说明怎么做，不会跑到一半才失败。

## 插件与平台

插件不必自己处理平台差异：桥接器把命令查找、子进程与 npm 调用通过 `config.runtime` 交给插件（见[开发桥接器插件](PLUGINS.md)）。Windows 上 `npm` 实际是 `npm.cmd`，Node 20 起不允许直接 spawn，`config.runtime` 已经处理；插件作者不必各自踩一遍。刷机工具也复用同一套命令查找（`tools/flash.mjs` 引入 `bridge/src/plugin-runtime.mjs`）。

## 已知缺口（未做）

- Windows 上不设置串口波特率 → 只支持接收器的原生 USB 变体。要么在文档里写死这条，要么引入 `serialport` 依赖补上 `SetCommState`，属于需要拍板的技术选型。
- Windows 上没有串口占用检测。
- Windows 上的 `--build` 依赖外部 IDF 环境，本工具不代为激活。

以上行为如有变更，请同步更新本文件与 `bridge/src/doctor.mjs`——两处说的是同一件事，别只改一边。
