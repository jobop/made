<div align="center">

# 码得 · made

**开源可扩展的 AI 伴侣硬件——AI 时代的电子宠物。**

说一句话，看见任务执行。任何 agent 只需一个插件，就能拥有自己的硬件。

[English](README.md) · [简体中文](README.zh-CN.md)

![License](https://img.shields.io/badge/license-MIT-green) ![Hardware](https://img.shields.io/badge/hardware-ESP32--S3-orange) ![Bridge](https://img.shields.io/badge/bridge-Node%20%E2%89%A5%2022-blue) ![Firmware](https://img.shields.io/badge/firmware-ESP--IDF%205.5-red)

**码得，代码触手可得** · **Made, when you want to make.**

</div>

---

## 为什么做码得？

编程 agent 住在终端和 IDE 里——看不见、听不到、离你很远。**码得**把它放到桌面上：一台掌心大小的设备，圆形触摸屏上的分层小精灵会呼吸、眨眼、说话，陪你跑完每一个任务。

它是 **AI 时代的电子宠物**：不是单一功能的玩具，而是靠插件生长的开放硬件。agent 不是一个写死的功能——它是一个你写的插件。接入你自己的 agent，它就拥有自己的形象、语音通道、会话记忆，以及桌面上的一个身体。

- **🐾 是宠物，不是遥控器。** 小精灵会待机呼吸、定时眨眼、说话对口型，任务状态驱动动效；主题与形象素材随插件包下发，无需刷固件。
- **🔌 一个插件 = 一位硬件住户。** 助手名单、图标、能力与近全屏精灵形象全部由桥接器运行时下发，新增 agent 不碰固件。
- **🛒 是生态，不是围墙。** 插件市场（`npx made-market`）分发 agent 运行时、管控插件和主题，专家市场注入人格。用户扩展自己的硬件，开发者触达用户的桌面。
- **🎙 语音优先。** 录音 → 转写 → 屏上回显 → 双击确认，确认之前什么都不执行。
- **🔗 四种接入方式。** USB 线直连、局域网 Wi-Fi、第二块板的接收器热点、公网 HTTPS 隧道——同一套协议、同一套设备接口。
- **🔓 完全开源。** MIT 协议：ESP-IDF 固件、Node.js 桥接器、网页工作台、可 3D 打印的外壳。不依赖任何云账号。

## 工作原理

```
        语音 / 按键                     设备接口（事件 ⇄ 命令队列）
  你 ───────────────▶ ┌─────────┐ ─────────────────────────▶ ┌──────────┐
                       │  码得   │    USB · Wi-Fi · 局域网    │  桥接器  │
  你 ◀─────────────── │  设备   │ ◀───────────────────────── │ (Node.js)│
        精灵屏幕       └─────────┘     接收器 · 公网隧道       └────┬─────┘
                                                                       │ CLI 会话
                                                          ┌────────────┼────────────┐
                                                          ▼            ▼            ▼
                                                        Codex       Cursor      你的插件…
```

**设备固件**是通用交互终端：录音、渲染助手精灵与答复、交换一套很小的事件/命令协议。**桥接器**跑在你的电脑上：托管网页工作台（`127.0.0.1:8787`）、提供设备 API（`8788`）、把 agent 插件作为 CLI 会话跑在你的 Git 仓库里，并把进度、确认与取消回传到设备。**一个任务就是一个会话**：显式创建，之后在同一上下文里连续对话。

## 硬件

| | |
| --- | --- |
| 参考板 | 微雪 `ESP32-S3-Touch-LCD-1.85B` |
| 屏幕 | 360 × 360 圆形电容触摸 |
| 芯片 / 存储 | ESP32-S3 · 16 MB Flash · 8 MB PSRAM |
| 可选接收器 | 第二块 ESP32-S3 或 ESP32-C3，作 Wi-Fi→USB 中继，适配受限网络 |
| 外壳 | [`3d-model/`](3d-model/) 内的 3D 打印模型（源文件、STL、3MF） |

产品与协议不限定屏幕形状——圆屏只是当前实现。

## 快速开始

**1. 启动桥接器**（Node.js ≥ 22，无 npm 依赖）：

```sh
cp bridge/config.example.json bridge/config.local.json
npm start
```

浏览器打开工作台 [http://127.0.0.1:8787](http://127.0.0.1:8787)，把 `bridge/config.local.json` 里的 `projects` 指到你的 Git 仓库。`bridge/demo-project` 是安全的试玩沙箱。

**2. 刷录设备**，一条命令：

```sh
npm run flash          # 扫描 → 认板 → 推荐目标 → 烧录
```

目标、认板规则与预构建镜像见 [docs/FLASH.md](docs/FLASH.md)。

**3. 连接**——锁屏上滑解锁，按需选择：USB 直连、局域网自动发现、手动/公网 HTTPS 地址、可选接收器。完整步骤见 [docs/CONNECTIONS.md](docs/CONNECTIONS.md)。

**4. 从市场安装 agent**：

```sh
npx made-market install codex cursor qoder workbuddy
```

**5. 配置语音**——任何兼容 OpenAI 转写协议的服务均可（URL + API Key + 模型，如 OpenAI 或硅基流动 SenseVoice）。详见 [docs/VOICE.md](docs/VOICE.md)。

**设备上的操作：** 左右滑切换助手 · `+` 新建任务 · BOOT 单击录音、双击确认、三击取消、长按约 1.2 秒回桌面。设备仅在前台且解锁时与桥接器通信。

## 写你自己的 agent——只差一个插件

一个 agent 就是一个 JavaScript 对象。放进 `~/made/plugins/`，重启桥接器，它就会出现在设备上——名字、图标、精灵、能力一并到位：

```js
export default {
  apiVersion: 1,
  kind: 'coding-agent',
  id: 'my-agent',                     // 稳定 ID——会话挂在它下面
  label: 'My Agent',
  capabilities: { session: 'native', model: true, cancel: true, progress: true },
  probe: () => ({ available: true }),
  async run({ job, project, signal, model, onProgress, onSession }) {
    onProgress('thinking…');
    onSession('my-native-session-id');  // 桥接器持久保存；下一轮经 job.sessionId 传回
    // 用 job.instruction 在 project.path 里干活；响应 signal 取消
    return { status: 'completed', result: 'Done.' };
  },
};
```

除了编程 agent，**管控插件**可以订阅设备事件（`boot.click`…）、经 `onTurnInput`/`onTurnOutput` 管道改写回合、创建任务、向设备下发命令（`caption.show`、`audio.play`、`theme.apply`…）。**主题插件**给宠物换装；**精灵素材包**给每个 agent 一个专属形象。

- 插件开发指南：[docs/PLUGINS.md](docs/PLUGINS.md)
- 精灵素材（分层立绘）规范：[docs/SPRITES.md](docs/SPRITES.md)
- 桥接器完整能力清单：[AGENTS.md](AGENTS.md)

## 插件生态

三个市场各管一层——只做包管理与登记，执行在桥接器：

| 市场 | 管什么 | 生效方式 |
| --- | --- | --- |
| **插件市场** | 码得本身：主题、管控插件、agent 运行时 | 安装进桥接器（`~/made/{plugins,themes}`）；上板资源经桥接器下发 |
| **技能** | agent 的可复用能力 | 沿用现成技能广场；按 agent 挂载 |
| **专家市场** | 注入 agent 的人格 | 安装进桥接器并挂到 agent，每次执行注入 |

市场命令行：

```sh
npx made-market init <目录> --kind agent    # 脚手架（agent / 管控 / 主题）
npx made-market validate <目录>             # 与服务端同一套校验
npx made-market publish <目录>              # 上架（需登录）
npx made-market install <包 ID>             # 安装进桥接器
npx made-market info <包 ID>                # 版本、校验和与安装位置
```

市场边界与归类规则：[docs/MARKETS.md](docs/MARKETS.md)。

## 仓库结构

```
made/               码得固件（ESP-IDF 5.5，Brookesia 桌面，码得应用）
receiver/           可选的 ESP32-S3 / ESP32-C3 接收器固件
bridge/             电脑桥接器：工作台 UI、设备 API、agent 运行时、示例
docs/               指南：插件、市场、精灵、连接、语音、刷机……
plugin-market/      插件包登记（只做包管理）
expert-market/      专家（人格）包登记（只做包管理）
3d-model/           3D 打印外壳（源文件、STL、3MF）
sim/                设备 UI 的主机模拟器（npm run sim）
tools/              刷机工具与开发脚本
```

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/PLUGINS.md](docs/PLUGINS.md) | 编写 agent 与管控插件：契约、会话、图标、回合管道 |
| [docs/MARKETS.md](docs/MARKETS.md) | 什么进哪个市场；打包规则 |
| [docs/SPRITES.md](docs/SPRITES.md) | 分层精灵素材规范：manifest schema、动效语义、体积限制 |
| [docs/CONNECTIONS.md](docs/CONNECTIONS.md) | 全部接入方式：USB、局域网、接收器、公网隧道、手机配网、配对安全 |
| [docs/VOICE.md](docs/VOICE.md) | 语音识别配置：兼容端点、预设、本机 Whisper |
| [docs/FLASH.md](docs/FLASH.md) | 刷机工具用法、认板规则、写盘地址表 |
| [docs/PLATFORMS.md](docs/PLATFORMS.md) | macOS / Linux / Windows 能力矩阵（`npm run doctor` 自检） |
| [docs/I18N.md](docs/I18N.md) | 中英双语的翻译边界与词典 |
| [made/](made/README.md) · [receiver/](receiver/README.md) | 固件构建与移植说明 |

## 测试

```sh
npm test          # 桥接器侧测试：任务、会话、配对、USB 分帧、语音上传……
npm run sim       # 设备 UI 主机模拟器（无需硬件即可截图）
npm run doctor    # 本机环境自检
```

## 参与贡献

欢迎提交 Issue 与 Pull Request。插件想法请从 [docs/PLUGINS.md](docs/PLUGINS.md) 和 `npx made-market init` 脚手架开始——多数扩展不需要动固件。改固件时请保证四种接入方式全部可用（见 [AGENTS.md](AGENTS.md)）。

## 许可证

[MIT](LICENSE)
