# 接入方式详解

码得支持四种接入方式，全部走同一套设备 API（事件上报 ⇄ 命令队列），换方式不需要重新配网以外的任何开发：

| 方式 | 适用场景 | 是否需要 Wi-Fi |
| --- | --- | --- |
| USB 线直连 | 无局域网、调试 | 否 |
| 局域网 Wi-Fi 直连 | 设备与电脑同网 | 是 |
| 接收器转发 | 公司 Wi-Fi 有准入/网页认证 | 仅接收器热点 |
| 公网 HTTPS 地址 | 跨网络远程使用 | 是（需可上网） |

配对流程对四种方式一致：电脑大盘点「添加码得」→ 板端选择目标 → **核对两端六位数字** → 电脑确认。

## 配对与凭据

- 电脑把配对令牌保存在 `state/paired-devices.json`；板子把令牌、电脑标识与地址保存在 NVS。
- **不需要在编译时填写电脑 IP 或设备令牌，也不需要设置 `VIBE_DEVICE_TOKEN`。**
- 配对接口：`POST /pair/request`、`GET /pair/status`；手动地址每次连接前还会用 `GET /pair/verify` 验证已存令牌与目标桥接机。
- 首次设备发现自动开放约两分钟配对窗口；窗口过期可在电脑大盘点「添加码得」重新开启。电脑防火墙需放行 UDP `8789`（发现）与 TCP `8788`（设备 API）。
- 已配对后，板子每次进入「码得」并解锁会先重新验证桥接机身份再复用令牌；验证失败会重新走六位码配对。换电脑在接入设置里选择 Wi-Fi 重新扫描即可，扫描、刷新和退出不会自动改配其他电脑。
- 仅「码得」应用在前台且已解锁期间，板子每约 12 秒发一次 `GET /device/heartbeat`；锁屏、退出应用或重启未解锁不上报，电脑按最近约 30 秒通信判断离线。

## 局域网 Wi-Fi 直连

1. 电脑与码得接入同一 Wi-Fi，启动桥接器（仓库根 `npm start`）。
2. 板端「码得」→ 齿轮 → 接入设置，选 **Wi-Fi 自动发现**，扫描后在码得列表点选目标电脑。
3. 核对两端六位数字，在电脑大盘确认。

## 手动地址与公网接入

在接入设置选 **手动地址**，填桥接机的 IPv4/域名、端口与协议：

- **可信局域网**：填电脑私有 IPv4，端口 `8788`，HTTP。明文 HTTP 可能被同网监听，只在可信网络使用。
- **公网**：必须选 **HTTPS**（通常 `443`），证书必须匹配所填域名或 IP（填公网 IP 时证书须含该 IP 的 SAN）。推荐填域名。**不要把明文 HTTP 端口直接映射到公网。**

### 电脑没有公网地址时（内网穿透）

电脑大盘的「公网接入」支持把**设备接口 `127.0.0.1:8788`** 发布为公网 HTTPS 地址；管理页 `127.0.0.1:8787` 始终只监听本机。穿透不会随桥接器启动自动开启，须在大盘手动开关。

- **Cloudflare 临时试用**：安装 [cloudflared](https://developers.cloudflare.com/tunnel/downloads/)，用 [Quick Tunnel](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/do-more-with-tunnels/trycloudflare/) 免域名试用，地址随机且可能变化。
- **Cloudflare 固定域名**：创建[远程管理 Tunnel](https://developers.cloudflare.com/tunnel/get-started/)，目标 `http://127.0.0.1:8788`，`HTTP Host Header` 覆盖项保持未设置（改成 `localhost` 会导致地址绑定验证失败）。在大盘填地址与 [Tunnel token](https://developers.cloudflare.com/tunnel/reference/tunnel-tokens/)；token 只存电脑 `state/tunnel-token`（0600）。
- **Tailscale Funnel**：安装并登录 Tailscale，tailnet 启用 MagicDNS、HTTPS 证书并允许 [Funnel](https://tailscale.com/docs/features/tailscale-funnel)。桥接器把 `8788` 以 HTTPS 443 发布到 `*.ts.net` 地址，码得无需安装 Tailscale。
- **ngrok**：安装 [Agent](https://ngrok.com/docs/gateway/agent/getting-started/)，大盘填 Authtoken；可留空地址自动分配。桥接器经 [Agent API](https://ngrok.com/docs/gateway/agent/api) 核对转发目标确为 `127.0.0.1:8788` 才展示。码得请求自带 [ngrok 跳过提示页的请求头](https://ngrok.com/abuse)。
- **花生壳**：在花生壳平台先创建外网 `443` → `127.0.0.1:8788` 的 HTTPS 映射并取得 [API Key](https://service.oray.com/question/50663.html)；桥接器只启停这条已存在且目标匹配的映射，不增删映射，开启后用随机挑战核验公网域名确实到达本机。

共同注意事项：

- **板子必须有可用网络并能访问对应公网地址**；公司网络禁止板子联网时，隧道无法替代，可让板子改用可上网的 Wi-Fi（如手机热点）。
- 公网 HTTPS 校验证书链、目标主机与有效期；板子在前台会先同步网络时间，未取得可信时间不会发请求（15 秒后报失败，单击 BOOT 重试）。
- 配对仍需电脑端确认；勿分享穿透密钥或设备令牌。
- 单独 [DDNS](https://www.noip.com/support/knowledgebase/i-have-added-a-hostname-what-do-i-do-with-it-now) 只更新域名到现有公网 IP，CGNAT 下无法替代内网穿透。

ngrok 与花生壳当前经模拟接口测试，真实账号联通待用户实测。

## USB 线直连（只需一块码得）

1. 数据线把码得插到电脑，板端 **接入设置 → USB → 保存**。电脑仍需运行桥接器；大盘「USB 连接」显示「码得 USB 直连」。
2. 首次按常规流程配对（六位码）；已配对则验证电脑身份后复用原配对。

特点与边界：

- 不需要 Wi-Fi、接收器或内网穿透；语音识别与 agent 都走电脑自己的网络。
- 走原生 USB Serial/JTAG 帧转发，**不是键盘模拟**，也不会自动启动电脑程序。
- USB 身份独立保存：换电脑或桥接器重启会重新验证，未验证不发送旧令牌。
- 仅应用前台、已解锁且已选 USB 时建立协议连接；退出应用会停止通信并取消进行中的录音，已执行的编程任务继续由电脑管理。拔线显示断开，重接验证后恢复。
- 固件运行日志保留在 UART0，USB 副控制台已关闭，避免日志混入数据协议。

## 可选接收器（第二块板）

公司 Wi-Fi 要求网页登录或设备准入时，用另一块 ESP32-S3（原生 USB）或 ESP32-C3 作接收器：它建立独立 WPA2 热点，把码得的 HTTP 请求经 USB 串口转发给电脑。

1. 按芯片与变体刷接收器固件（别刷进码得主机）：S3 见 [receiver/release/FLASH.md](../receiver/release/FLASH.md)；C3 原生 USB 用 `receiver-c3-usb`，WCH 转串口用 `receiver-c3`（见 [receiver/release-esp32c3/FLASH.md](../receiver/release-esp32c3/FLASH.md)）。**刷错变体桥接器收不到数据**；Windows 上只支持原生 USB 变体（[平台支持](PLATFORMS.md)）。
2. 保持桥接器运行，大盘「USB 连接」查看热点名称与 8 位密码（WPA2）。macOS/Linux 自动发现限定 Espressif `303a:1001` 与 WCH `1a86:55d3`；如需指定端口：`VIBE_USB_PORT=/dev/cu.usbmodemXXXX npm start`。码得主机若也在 USB 直连，请先退出该模式。
3. 板端 **接入设置 → 接收器**，扫描并点选热点，输入大盘显示的密码保存。连接后使用固定地址 `http://192.168.4.1:8788`，无需填电脑 IP。
4. 未配对时在电脑大盘「添加码得」核对六位码。同一台电脑上，两种传输方式复用经验证的配对并同步令牌更新与撤销。

边界：

- 接收器只是 USB/Wi-Fi 转发器，**完整功能仍需电脑运行桥接器**；热点本身没有互联网。
- 设备仅在前台且已解锁时临时连接收器热点，退出「码得」后恢复原 Wi-Fi。
- C3 实体板已完成烧录、校验及 USB 握手；热点连接、配对、语音全链路待完整联调。

## 手机配置（无电脑参与）

码得 → 齿轮 → 连接 → **手机配置**。手机扫屏幕二维码加入 `Made-Setup-xxxx` 临时热点（通常自动弹出配置页，否则浏览器打开 `http://192.168.8.1`；也可手动输热点密码连接）。

网页可填写普通 Wi-Fi、接收器热点信息，或粘贴桥接器完整公网 HTTPS 地址；点「保存并连接」后码得关闭热点并应用设置，再按原流程核对六位配对码。

- 该页由码得自身提供，无需先连上电脑，也不需要手机应用。
- 入口仅在配置页打开期间有效，10 分钟自动关闭；返回或长按 BOOT 退出也会关闭。
- 语音 API Key、模型与任务管理仍在电脑工作台配置。
