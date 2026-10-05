# 码得 ESP32-C3 UART 接收端烧录文件

本目录面向 **ESP32-C3、4 MB Flash、板载 WCH USB 转串口连接 UART0** 的接收端。当前实体板 USB ID 已识别为 `1a86:55d3`，macOS 上本次端口为 `/dev/cu.usbmodem5B7A1277141`；换电脑或 USB 口后应重新核对端口。已在这块 C3 上烧录并校验，电脑桥接器已实际收到 USB 握手并显示热点信息。码得 → 接收端热点 → USB → 电脑助手的完整链路仍待实测。

固件使用 `CONFIG_VIBE_RECEIVER_UART_TRANSPORT=y`，UART0 TX 为 GPIO21、RX 为 GPIO20，运行波特率为 **921600，8N1**；应用 console 为 `NONE`，bootloader 日志关闭。默认配置见上级目录的 `sdkconfig.defaults.esp32c3`。本目录镜像不能用于 ESP32-S3 或码得主机（微雪 1.85B）；原生 USB 接线的 C3 需关闭 UART 配置并重新编译，见上级 README。

## 首次烧录

先停止占用接收端串口的桥接器和串口监视器。在本目录执行，端口替换为实际接收端端口：

```sh
python -m esptool --chip esp32c3 -p /dev/cu.usbmodem5B7A1277141 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-size 4MB \
  0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 vibe_receiver.bin
```

三份镜像的写入地址分别为：`bootloader.bin` → `0x0`、`partition-table.bin` → `0x8000`、`vibe_receiver.bin` → `0x10000`。首次刷写其他用途的开发板会覆盖原程序，需保留原固件时先做好备份。上面 `460800` 是烧录波特率，与启动后协议使用的 `921600` 分开设置。

若不能自动进入下载模式，按住 BOOT、点一下 RESET，再松开 BOOT 重试。烧录后重启桥接器；电脑页面应显示接收端的热点名称和密码。若码得主机也接在电脑上，先退出其 USB 直连模式，或启动桥接器时用 `VIBE_USB_PORT` 指定接收端端口。

## 后续升级

使用同一分区表升级时，只写应用分区即可保留 NVS 数据；旧版长密码的一次迁移规则见下文：

```sh
python -m esptool --chip esp32c3 -p /dev/cu.usbmodem5B7A1277141 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  0x10000 vibe_receiver.bin
```

接收端首次启动生成并保存 **8 位数字**热点密码。升级时，仅把没有 `password_ver` 标记且符合旧版字符集的 16 位自动生成密码迁移一次，保存八位数字及 `password_ver=2`；SSID 和其他 NVS 数据保留。已有版本标记的密码、已有八位数字及其他格式密码不变，完整规则见[接收端 README](../README.md#从旧热点密码升级)。迁移后需在码得接收端设置中更新一次新密码，之后重启不会再变化。

电脑仍需运行桥接器，管理页面为 `http://127.0.0.1:8787`。在码得「设置 → 接入设置 → 接收端」填入页面显示的热点名称、密码，保存后核对六位码；随后验证任务列表、语音确认/取消、会话删除和重连。串口识别、编译或烧录成功不能替代这些端到端测试。
