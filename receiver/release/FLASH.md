# 码得 USB 接收端烧录文件

这三个文件由 ESP-IDF 5.5.5 为通用原生 USB ESP32-S3 编译。本目录的 S3 版本已重新编译校验，尚未在独立 S3 接收端实测。ESP32-C3 UART 版本另见 `../release-esp32c3/`。不要把这些文件烧录到现有的码得主机（微雪 ESP32-S3-Touch-LCD-1.85B）；码得主机使用 `made/release/` 中的镜像。

首次刷写接收端时，在本目录执行（将端口替换成该开发板的原生 USB 串口）：

```sh
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 2MB --flash_freq 80m \
  0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 vibe_receiver.bin
```

若端口不能自动进入下载模式，请按住开发板的 BOOT 再按一下 RESET，然后松开 BOOT 重试。双 USB 口开发板请选择原生 USB/OTG 口。接收端首次启动会在 NVS 生成并保存 **8 位数字**热点密码；电脑桥接器连接 USB 后，大盘会显示热点名称和密码。后续升级只写 `0x10000 vibe_receiver.bin` 可保留 NVS。

本版会把无版本标记且符合旧版字符集的 16 位自动生成密码迁移一次，保存八位数字和 `password_ver=2`；SSID 和其他 NVS 数据保留。已有版本标记的密码、已有八位数字及其他格式密码不变。迁移后请在码得接收端设置中更新一次大盘显示的新密码，之后重启不会再变化。完整规则见[接收端 README](../README.md#从旧热点密码升级)。
