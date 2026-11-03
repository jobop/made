# 哈基米机 PetUI 组件系统研究笔记（板载组件插件 V2 设计输入）

> 源码来源：github.com/YizhengWw/HachimoDock（浅克隆在 /tmp/hachimo，2026-11-04 快照，v0.1.168 / 固件 0.7.103-p4-r2）。
> 结论先行：**它不是浏览器，也不跑 H5**。是一套「声明式 JSON 组件 + 受控 PNG 素材 + 固件受限解释器」的体系，Agent 按固定契约生成组件，校验通过才允许发布。

## 1. 整体架构

```
Agent（Codex/Claude）调 petui Skill
  → 生成 .clawpkg 组件包（纯声明式 JSON + PNG，禁止任何代码）
  → 校验器 validate_generated_widget.py（结构/素材/字节预算/能力协商）
  → 玩法自测 smoke_test_widget_game.py（从真实按键映射探索可达局面）
  → 原子发布 ~/.claw-pet/components/library/<id>/<内容hash>/
  → 桌面端组件中心「保存并同步」经 USB 下发设备
  → P4 固件 PetUI 运行时解释执行（pet_p4_miniapp.c，5500 行）
```

关键决定：**第三方组件不能执行任意代码、不能联网、不能访问文件**。游戏逻辑全是固件解释器能理解的声明式原语。

## 2. 组件包结构（.clawpkg）

```
<id>/
├── component.json      # id/name/version/description, kind: "game"|"tool"
├── negative-screen.json # 默认画面预览（必须与 runtime 初始态一致）
├── buttons.json        # ≤8 条输入映射（action/control/event/label）
├── runtime/widget.json # 完整玩法定义，紧凑 JSON ≤4095 字节
├── assets/             # 只有最终 PNG（横向 sprite sheet，单个 ≤128KiB）
└── share.json
```

## 3. 运行时引擎（p4-bounded-runtime-v4）的硬预算

| 维度 | 上限 |
|---|---|
| 变量 | ≤8 个，type 仅 int/string |
| 状态 | ≤6 个状态、≤4 个页面 |
| transitions | ≤12 条；tick ≤8 条 |
| 每条动作 | set+inc ≤4 项 |
| widget.json | ≤4095 字节（紧凑） |
| Dashboard 槽位 | title 60B / headline 156B / footer 156B 等，按 UTF-8 字节计 |
| Scene | 网格 4-16（宽高）、≤12 实体、速度 -4..4、≤20 规则、tick 100-2000ms |
| 精灵 | ≤4 个 PNG、每帧 8-64px、全部帧合计 ≤4096 像素、1-20fps |

没有 if/else、表达式、字符串拼接、CSS。Dashboard 只允许字面量/switch_state/switch_page/fmt_mmss/var/pct_of。

## 4. 最有价值的发现：俄罗斯方块是固件原生预设

- 通用 Scene 网格上限 16×16，**装不下 10×20 的俄罗斯方块**。
- 它们自己的「像素方块」（block-combo）runtime 里用的仍是 `game.type: "blocks"`——**玩法逻辑内置于固件**（pet_p4_game.c，1302 行），组件包只配置：tick_ms=480、六个 action 的按键映射、皮肤（assets 方块贴图 + surface 主题）、最高分持久化（persist best）、连发键（input repeat）。
- 同理 flappy/snake 也是原生预设。**纯声明式 Scene 表达不了它们自家的招牌游戏**——声明式与原生玩法的边界比宣传的更紧。
- 对我们的启示：设计规范时要么把网格上限放大（S3 内存够呛，要算），要么学它把 2-3 个经典玩法做成固件预设、组件包做「皮肤+按键映射+参数」的定制层。

## 5. 能力协商（值得照抄）

校验器接受设备能力 JSON（widgetRuntime/widgetScene/widgetSprites/touchInput.ready/…），组件用可选引擎键声明依赖（p4-input-v1 连发、p4-motion-v1 蓄力物理、p4-pointer-v1 触屏切果、p4-data-list-v1 行情、p4-media-v1 播放器、p4-instrument-v1 音效）。设备能力不含的引擎 → 拒装，不允许「靠版本号假定支持」。

## 6. 反虚构与自测门槛（Agent 生成质量的关键）

- 生成前先写「机制清单」：目标闭环/实体变化/每个输入的即时可见结果/结算与重开。
- 禁止复制内置组件改名冒充新组件；优化必须保留原 component.json.id（ID 是版本链身份，内容 SHA-256 短哈希是版本）。
- 玩法自测从 buttons.json 真实映射出发探索可达局面：开始 → 每个输入有即时画面反馈 → tick 真推进 → 得分可达 → 结算可达 → 可重开。只改速度字段/文案不算通过；**发布器会再跑一遍，失败包进不了正式库**。
- 文案与 runtime 一致性：声称移动/碰撞/射击，runtime 必须有对应机制。

## 7. 主题引擎

visualStyle（clean 默认/pixel 需用户明确要求）、palette 8 色、layout（arcade/scoreboard/tool）、安全图标集、surface（背景/字体/排版）。没有生成能力的 Agent 必须退回语义形状并说明理由，禁止矩形冒充主体。

## 8. 对我们板载组件插件（V2）的设计输入

1. **声明式 JSON + 字节预算 + 生成前校验 + 自测门槛**四件套直接照抄，这套是它质量的根基；
2. 网格/实体预算要按我们 S3 的内存重新算（它 32MB PSRAM，我们 8MB）；
3. 经典玩法走「固件预设 + 组件包定制（映射/皮肤/参数）」分层，别硬上纯声明式；
4. 能力协商机制照抄——我们设备形态更多（触摸屏、编码器、语音），能力清单会更长，更要协商制；
5. 下载/安装链路我们复用现有市场管线 + 桥接器 /device 素材分片通道（注意 SPIFFS 31 字节文件名限制对组件资源命名的影响）；
6. 它桌面端「组件中心、双态（已发布/已同步设备）」的产品语义也值得抄——我们的 App 市场页正好承接。

## 9. 规模参考

- 固件：pet_p4_miniapp.c 5528 行（运行时+解析+校验）+ renderer.c 5533 行 + game.c 1302 行（三个原生预设）+ 输入/触摸/数据等，main 下 .c 合计 ~18.6 万行（含字体/资源数据）。
- PC：validate_generated_widget.py + smoke_test_widget_game.py + publish_generated_widget.py + 各子系统 reference 文档（contract.md 363 行是唯一真源）。
- 内置组件 22 个（builtin-clawpkgs），覆盖行情/木鱼/待办/电脑状态/番茄钟/贪吃蛇/方块/flappy 等。
