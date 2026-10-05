# 码得 USB 接收端固件

面向 ESP32-S3 和 ESP32-C3 开发板，使用 ESP-IDF 5.5。S3 默认走原生 USB Serial/JTAG；C3 提供通过板载 WCH USB 转串口连接 UART0 的配置，也可按布线选择原生 USB。这是码得的可选接收端；码得产品名不限定主机形状，当前主机实现为微雪 ESP32-S3-Touch-LCD-1.85B（360×360 圆形触摸屏）。接收端创建独立的 2.4 GHz WPA2 热点，码得连接这个热点后，将现有 `/pair/*`、`/device/*` HTTP 请求发送到 `http://192.168.4.1:8788`。接收端按块转发到电脑的 USB 串口适配器；语音和响应都不需要 PSRAM 整段缓存。

接收端只是传输设备。电脑仍需运行本项目的本地桥接进程，才能调用本机 Codex、Cursor、Qoder 等工具、访问项目目录、保存任务，并提供任务管理页面。接收端不直接执行电脑上的进程，也不向企业 Wi-Fi 或公网发起连接。

## 硬件和构建

- **ESP32-S3 原生 USB**：若板上有两个 USB 口，电脑连接标为 `USB`、`OTG` 或直连 GPIO19/20 的原生 USB 口。默认 S3 镜像不使用外置 USB 转串口的 `UART` 口。固件日志在 UART0，USB 副控制台关闭，避免日志混入协议。
- **ESP32-C3 板载 WCH USB 转串口**：当前实体板已识别为 ESP32-C3、4 MB Flash，USB ID 为 `1a86:55d3`。接收端通过 UART0 通信，TX 为 GPIO21、RX 为 GPIO20，波特率 **921600，8N1**。配置 `CONFIG_VIBE_RECEIVER_UART_TRANSPORT=y` 后，不使用芯片原生 USB；应用 console 必须为 `NONE`，bootloader 日志也关闭。`sdkconfig.defaults.esp32c3` 已提供这些默认值。已完成实板烧录、镜像校验和电脑 USB 握手；完整无线配对与语音链路仍待实测。
- **ESP32-C3 原生 USB**：仅适用于 USB 数据口直连 GPIO18（D-）和 GPIO19（D+）的板子。在 `menuconfig → Made receiver` 关闭 `Use UART0 through a USB-to-serial adapter`，并把 console 改为 UART0、关闭 USB 副控制台。不要把 WCH UART 版本直接用作原生 USB 版本。

在 ESP-IDF 5.5 环境构建 S3：

```sh
cd receiver
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash
```

构建 C3 UART 版本，使用独立构建目录和配置文件，保留已有 S3 配置：

```sh
cd receiver
idf.py -B build-c3 -D SDKCONFIG=sdkconfig.c3 \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c3" \
  -D IDF_TARGET=esp32c3 build
idf.py -B build-c3 -D SDKCONFIG=sdkconfig.c3 -p /dev/cu.usbmodemXXXX flash
```

预构建镜像分开存放：S3 见 [`release/FLASH.md`](release/FLASH.md)，C3 UART 见 [`release-esp32c3/FLASH.md`](release-esp32c3/FLASH.md)，两个芯片的镜像不能混刷。端口名以本机实际设备为准；macOS 通常为 `/dev/cu.*`，Linux 为 `/dev/ttyACM*` 或 `/dev/ttyUSB*`。桥接器自动发现只接受 `303a:1001`（Espressif 原生 USB）及本次确认的 `1a86:55d3`（WCH），并在验证设备发出的 `hello` 后开始通信。其他 USB 转串口芯片可手动指定 `VIBE_USB_PORT`，且必须与固件的 UART 接线匹配。

桥接器将串口设置为 raw 921600 8N1。UART 版本必须与此波特率一致；USB Serial/JTAG 的实际传输速率不由串口波特率决定。若码得主机也插在同一电脑上，先退出它的 USB 直连模式，再测试接收端；必要时用 `VIBE_USB_PORT` 固定接收端端口。

S3 版本已在 `espressif/idf:release-v5.5` 环境完成编译；C3 已完成芯片、Flash 和 WCH USB 设备识别。编译通过或发现串口不代表端到端验证通过，实际热点连接、配对和语音任务须另行测试。

## 首次使用

1. 接收端首次启动时，用硬件随机源生成 **8 位数字**热点密码并存入 NVS；不足八位会保留前导零，输入时需填完整八位。SSID 是 `VibeReceiver-` 加 AP MAC 尾部六位十六进制字符。普通重启不会重新生成密码。
2. 电脑桥接器打开 USB 串口后，接收端会在空闲时每 5 秒发送 `hello`，其中有 SSID 和密码。每次转发请求前，接收端再次发送 `hello`，并等待电脑回复 `ready` 后才发送请求与录音，避免电脑程序尚未启动或重启时丢请求。电脑桥接器仅在本机管理页显示密码用于码得配网，不应写入日志、状态文件或公网设备接口。
3. 在设备的「码得」应用中打开「设置 → 接入设置 → 接收端」，自动扫描并点选接收端热点，再输入电脑本机管理页显示的 8 位数字密码，点「保存」。也可选择「手动填写」。整个流程在码得内完成，无需桌面 Settings。码得会临时连接接收端热点，并固定访问 `http://192.168.4.1:8788`；无需手动输入 IP、端口或开内网穿透。再次进入设置时 SSID 会预填，若同一 SSID 的密码栏留空，会沿用已保存的密码。
4. 在电脑管理页开启添加码得，按原有六位码流程核对并确认配对。本版本不伪装原 UDP 自动发现；首次及后续重连都使用接收端固定地址及现有 `/pair/verify` 校验。

接收端 SSID、密码保存在码得独立的 NVS 项，不覆盖原有 Wi-Fi 凭据。连接同一台电脑时，两种传输方式复用经验证的配对，令牌更新或撤销时同步处理；如果连接的是不同电脑，则各自保存配对身份。退出「码得」应用后，设备会恢复原 Wi-Fi；若接收端连接失败，码得保持接收端模式并提示核对热点，不会擅自把任务发给其他电脑。用户可在接入设置中手动切回「同 Wi-Fi」或「公网」；没有接收端时，原有两种连接方式照常使用。

### 从旧热点密码升级

升级时仅迁移**未保存 `password_ver` 标记、长度恰为 16、所有字符均来自 `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`** 的旧版自动生成密码：用硬件随机源生成一次八位数字，写回 `password`，然后记录 `password_ver=2`。不擦除 NVS、不更改 SSID。其他已有密码（包括其他格式的手动写入值、已有版本标记的值和八位数字）保留；旧版未提供自定义密码界面，因此无版本且恰好符合旧字符集的值按旧版生成密码处理。

迁移后电脑大盘会显示新密码，请在码得接收端设置中更新一次；后续重启或刷同一版本不会再改变。若密码已保存而版本标记写入前掉电，下一次启动也会保留这串八位数字，仅补齐标记。

生成与迁移规则可在电脑上验证：

```sh
cc -std=c11 -Wall -Wextra -Werror tests/password_test.c -o /tmp/made-receiver-password-test
/tmp/made-receiver-password-test
```

## USB NDJSON 协议 v1

按固件配置，协议口为 USB Serial/JTAG CDC 或经 USB 转串口芯片连接的 UART0；两者使用相同协议。UTF-8 JSON 每行一帧，以 `\n` 结束，**包括换行符每行最多 2048 字节**。接收端同时只处理一个 HTTP 请求；`id` 是递增的十进制 `uint32`，每次重启从 1 开始。原始数据按最多 1024 字节分块，再 Base64 放进 `chunk`。接收端请求体上限是 **1,100,000 字节**（与现有电脑语音接口一致），电脑响应体上限是 **262,144 字节**；从转发请求开始到完整响应结束，普通请求截止时间为 30 秒，`POST /device/voice` 为 150 秒，以覆盖最长 120 秒的本地语音转写和 USB 上传时间。

接收端发送：

```json
{"type":"hello","version":1,"ssid":"VibeReceiver-A1B2C3","password":"<NVS 中的随机密码>"}
{"type":"request","id":1,"method":"POST","path":"/device/voice?provider=codex&projectId=demo&sessionId=...","authorization":"Bearer ...","contentType":"audio/wav","length":32044}
{"type":"data","id":1,"chunk":"<最多 1024 原始字节的 Base64>"}
{"type":"end","id":1}
```

电脑回复：

```json
{"type":"ready","version":1}
{"type":"response","id":1,"status":201,"contentType":"application/json; charset=utf-8","length":42}
{"type":"data","id":1,"chunk":"<最多 1024 原始字节的 Base64>"}
{"type":"end","id":1}
```

`ready` 是对 `hello` 的确认，须使用支持此确认的当前电脑桥接器。接收端每次发出请求头之前最多等候 3 秒；未收到确认则报错，让码得重试。空闲时的 `hello` 同样会得到 `ready`；接收端在下一轮空闲 hello 前，持有转发锁清理旧回复，避免长时间空闲堆满串口接收缓冲，不影响正在处理的请求。电脑串口读取会按批次排空当前数据，正在传输时加快读取，避免长录音被固定的轮询间隔拖慢。

退出「码得」应用后，设备会离开接收端热点。接收端检测到断开，向电脑发送 `{"type":"cancel","id":1}` 中断对应请求；电脑立即停止该请求的在线时间刷新，并取消上游语音识别。迟到的识别结果不会生成待确认指令。已开始执行的 Coding Agent 任务不受退出影响，仍需三击 BOOT 或电脑页面明确取消。

`request.length` 和 `response.length` 都是**解码后的原始字节数**，`end` 前各 `data` 解码字节数之和必须完全一致。`GET` 等空请求也发送 `end`。`authorization` 和 `contentType` 缺失时使用空字符串；`path` 包含查询参数。接收端保留电脑回复的 HTTP 状态码和 Content-Type，以 HTTP chunked 响应传回码得。单请求的串口写入、读取、数据长度或协议校验失败时，接收端返回 502；超时返回 504。若 HTTP 响应已经开始，则关闭连接让码得察觉失败。USB 重连后无需重新烧录，接收端继续每 5 秒发 `hello`。

目前只转发码得用到的 GET 和 POST，路径必须以 `/pair/` 或 `/device/` 开头。电脑代理需将原始 `Host` 语义保持为 `192.168.4.1:8788`，并仅对 USB 回环代理允许该 authority；现有配对验证会校验此地址，不能简单改成 `127.0.0.1:8788`。

## 已知边界

- 接收端热点不提供互联网；码得连接它时，小智、天气等需要互联网的应用不能使用该热点访问云服务。电脑上的 Coding Agent 仍可通过电脑自身网络访问其服务。
- ESP-IDF 的 USB Serial/JTAG 是固定功能 CDC 串口，不是 USB 虚拟网卡；因此本版本的接收端不会仅凭 USB 插入就在电脑浏览器提供网页。管理页由运行在电脑上的桥接器提供。
- 本固件不会进行 UDP V1/V2 自动发现、ESP-NOW 或 BLE 配对。码得的「接收端」模式使用固定本地地址，继续沿用既有 HTTP 六位码配对和令牌。
- 通用开发板的供电、USB 口布线及系统串口权限各异，需在目标板和目标电脑上完成实测。

官方资料：[ESP32-S3 SoftAP 示例](https://github.com/espressif/esp-idf/blob/master/examples/wifi/getting_started/softAP/README.md)、[USB Serial/JTAG 说明](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-guides/usb-serial-jtag-console.html)、[硬件随机数说明](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/random.html)。

## 网页与启动方式

接收端通过原生 USB 串口或板载 USB 转串口向电脑转发数据，不内置大盘 HTML，也不自动打开浏览器。电脑仍需安装并运行本项目的桥接程序，再打开 `http://127.0.0.1:8787`。网页、任务管理和配置服务都由电脑进程提供；接收端不能直接运行电脑上的 Coding Agent。
