# 助手形象素材（sprite）开发规范

助手页的小精灵形象不由固件内置，而是**随 agent 运行时插件包下发**：包里带一个
`sprite/` 目录（分层 PNG + manifest），桥接器把素材清单暴露给码得，码得按版本号
分片拉取、落盘缓存。换形象 = 换插件包里的素材，**不需要刷固件**。

本文是素材包的格式真源。改动本文时，三处必须同步：本仓库 [插件开放能力清单](./standards/03-plugin-capabilities.md)、市场 CLI 脚手架（made-market 仓库 `cli/`，`npx made-market init --kind agent`
生成的模板）、市场网站 [开放能力说明页](https://made-market.link/capabilities.html)。

## 归属与优先级

形象素材是 **agent 的属性**，挂在 agent 运行时插件包上，不进主题插件、不进专家包：

| 优先级 | 来源 | 说明 |
| --- | --- | --- |
| 1 | agent 插件包的 `sprite/` | 本文规范的主体；版本号变化即重新拉取 |
| 2 | 固件内置兜底形象 | 未连接桥接器、包里没带 sprite、或素材拉取失败时显示 |

**内置兜底形象（绿豆）就是本格式的第一个实例**：它由
`assets/branding/split-bean-layers.py` 把立绘拆成 body / eyes(两帧) / mouth(两帧)
编进固件（`img_sprite_bean.c` + 坐标头文件），与下发的素材走同一条提交与驱动
管线。固件里没有任何写死的五官坐标——眨眼/说话都是特征层切帧，眼位来自
manifest 的 `x`/`y`。给任何新助手做形象，照本文规范提供分层 PNG 即可，
不需要碰固件。

主题插件的配色仍作用于界面底色和文字；旧主题包 `lock.*` 资产的兼容处理见
[vibe_theme](../made/components/vibe_theme/vibe_theme.cpp)。

## 素材包结构

```
<agent-package>/
  market.json          # kind: "agent"，见 made-market 脚手架
  plugin.mjs
  sprite/
    manifest.json      # 必需，见下文 schema
    body.png           # 层素材，文件名自定，manifest 里引用
    eyes-open.png
    eyes-closed.png
    mouth-closed.png
    mouth-open.png
```

`sprite/` 目录**可选**。没有它，这个助手用固件内置兜底形象。

## 两种素材形态：声明式整帧（推荐）/ 分层叠加（老格式）

**整帧状态动画（`frames` 表，推荐新素材用）**：每一帧就是整只角色，
素材明说「哪个状态播哪几帧、多快、眨眼怎么插」，固件只照表播放，
**不推断素材结构**。整只形象只有一个图层，状态切换只换一张图，
物理上不可能出现「一层一只鸟」的重影：

```jsonc
{
  "version": 16,
  "canvas": { "w": 240, "h": 240 },
  "frame_ms": 180,                       // 顶层默认每帧时长（30–2000）
  "frames": {
    "idle":    ["idle.png"],             // 待机；至少要声明 idle
    "blink":   { "frames": ["blink.png"],
                 "period_ms": 4000, "hold_ms": 120 },  // 周期性插入（眨眼）
    "talking": ["talk-0.png", "talk-1.png", "talk-2.png"],  // 说话循环
    "running": "…", "waiting": "…", "success": "…", "sad": "…", "offline": "…"
  }
}
```

- 状态名取 `idle/talking/running/waiting/success/sad/offline`，外加 `blink`
  （周期性插入的一条短轨）。未声明的状态回落到 `idle`
- 数组是简写（默认 `frame_ms`）；对象可加 `ms/period_ms/hold_ms`
- 总帧数 ≤12（设备内存），单文件 ≤256KB、整包 ≤512KB
- 有 `frames` 时 `layers` 可以省略；两套二选一

**分层叠加（`layers`，老素材）**：`body` + 特征层（eyes/mouth 小裁剪块）
按 z 序叠加。下述特征层契约只适用于这种形态。

## 最小可用与推荐分层（layers 形态）

- **最小可用**：只有一层 `body`（一张完整立绘）。能动（呼吸/轻摆/情绪节奏），
  但不会眨眼、不会张嘴——适合快速验证
- **推荐**：`body` + `eyes`（open/closed 两帧）+ `mouth`（closed/open 两帧）。
  眨眼 = 固件按 `blink.period_ms` 切换 eyes 层的两帧；说话 = 按 `talk` 切换
  mouth 层两帧。**位置全部由 manifest 的 `x`/`y` 声明，固件零写死坐标**
- **进阶（情绪表）**：把 `eyes`/`mouth` 层的 `frames` 扩到最多 8 帧
  （专注/惊讶/开心/垂眼……），配 `emotions` 表按助手状态切帧——见下文，
  不加层、不改坐标，老固件安全忽略
- **特征层硬契约**：`eyes`/`mouth` 各帧的 PNG **只含特征像素，其余全透明**；
  `body` 必须把五官擦掉。设备按 z 序叠加各层——若把整幅立绘拷贝当特征层用，
  上层会永远盖住下层的表情帧（实测：固件在切帧、屏幕永远睁眼）
- **特征区互斥且有界**：嘴区不能与眼区重叠、面积 ≤25% 画布。实测反例：
  把「最大差异簇」当嘴区，全身描边+瞳孔全被烤进嘴层——嘴层 22k 像素
  遍布全图（每 tick 全画布混合 → 动效卡顿），且烤进的瞳孔永远盖住
  眼层 → 眨眼失效
- **层图裁到内容 bbox**：manifest 的 `x`/`y` 写裁剪后左上角的画布坐标。
  全画布图层在设备上多付解码内存与混合面积（360×360 RGB565A8
  ≈ 380KB/张）；同层多帧必须裁到**并集** bbox，否则共用 `x`/`y` 会错位
- **软边不要带底色纱**：特征层掩码高斯软边的低 alpha 部分会把源帧底色
  半透明盖到 body 上，两帧间有量化漂移时显「贴片缝」——软边 alpha 低于
  ~60 的像素直接置 0
- 内置绿豆就是这么拆的，可作为拆层参考：
  `assets/branding/split-bean-layers.py`（含把平涂风格立绘自动拆层的做法：
  暗色连通域找五官 → 修平进 body → 特征抠成两帧）

## manifest.json schema

```json
{
  "version": 3,
  "canvas": { "w": 240, "h": 240 },
  "layers": [
    { "id": "body", "file": "body.png", "x": 0, "y": 0, "z": 0 },
    { "id": "eyes", "frames": ["eyes-open.png", "eyes-closed.png"],
      "x": 120, "y": 150, "z": 1,
      "blink": { "period_ms": 4000, "closed_ms": 120 } },
    { "id": "mouth", "frames": ["mouth-closed.png", "mouth-open.png"],
      "x": 150, "y": 220, "z": 2, "talk": true },
    { "id": "accessory", "file": "accessory.png", "x": 0, "y": 0, "z": 3,
      "alert": "pulse" }
  ],
  "motion": {
    "breath": { "scale_pct": 108, "period_ms": 3000 },
    "sway_deg": 3,
    "sway_period_ms": 1800
  },
  "emotions": {
    "idle":    { "eyes": 0 },
    "talking": { "mouth": "cycle" },
    "running": { "eyes": 2 },
    "waiting": { "eyes": 3, "mouth": 1 },
    "offline": { "eyes": 1 },
    "success": { "eyes": 4, "mouth": 2 },
    "sad":     { "eyes": 5, "mouth": 3 }
  },
  "commands": { "A1": "fall" }
}
```

字段说明：

| 字段 | 必需 | 说明 |
| --- | --- | --- |
| `version` | 是 | 整数，**单调递增**。码得只认版本号，变了才重新拉取 |
| `canvas` | 是 | 画布尺寸。**设备屏 240×240**（设备帧槽按 240×240 定容，超了单帧解码回退堆分配，PSRAM 碎片化）；`x`/`y` 是层左上角在画布上的位置 |
| `layers[].id` | 是 | 层标识。`body` / `eyes` / `mouth` / `accessory` 是保留名，其余自定义 |
| `layers[].file` / `frames` | 二选一 | 单帧层用 `file`；动画层用 `frames`（按帧序循环或按状态切换） |
| `layers[].x` / `y` / `z` | 是 | 位置与叠放次序，z 小的在下 |
| `layers[].blink` | 否 | 眨眼节奏：每 `period_ms` 闭眼 `closed_ms`。只对 `eyes` 层生效；闭眼帧约定为 `frames[1]` |
| `layers[].talk` | 否 | `true` 时录音/说话状态下循环播 `frames`（嘴型） |
| `layers[].alert` | 否 | `pulse`：待确认任务时该层做透明度脉冲提醒 |
| `motion` | 否 | 整体动效参数；缺省用固件默认值（呼吸 8% / 摆动 3° / 1800ms） |
| `emotions` | 否 | 情绪表：把助手状态映射到各层的帧（见下节）。缺省时所有状态共用第一套帧（兼容旧素材） |
| `commands` | 否 | 命令映射（两级绑定模型的「命令绑素材」侧）：抽象命令号 `A1`–`A9` → `actions` 表动作名，纯编号不定语义。声明后素材只认命令号，语义名直发废弃。详见「跨角色平台契约」 |

## 情绪表（emotions）

`emotions` 让同一个助手在不同状态下换表情——关键是**换的是既有层显示哪一帧**，
不加新层、不改坐标。结构：`状态 → 层id → 帧选择`，帧选择是帧序号（0 起）
或字符串 `"cycle"`（循环该层全部帧，同 `talk` 节奏）。

- 状态名（与固件情绪一一对应）：`idle` 待机 / `talking` 录音·说话 /
  `running` 任务运行中 / `waiting` 任务待确认 / `offline` 断连 /
  `success` 任务刚完成（瞬时约 3.5s）/ `sad` 任务刚失败·取消（瞬时约 3.5s）
- 未写的「状态×层」组合回退默认行为：`blink` 层第一帧 + 眨眼节律、
  `talk` 层第一帧、其余层第一帧
- `blink` 层的闭眼帧固定用 `frames[1]`；某状态选中的帧**就是** `frames[1]`
  时（如 `offline` 选闭眼帧当睡觉），该状态下不再眨眼
- `talk` 层在说话/运行中默认循环全部帧；情绪条目给了帧序号则固定显示该帧
  （如 `waiting` 固定张嘴），写 `"cycle"` 恢复循环
- 桥接器会校验：状态名、层引用、帧序号越界；`emotions` 引用的帧必须已在
  某层的 `frames`/`file` 里声明（不新增文件引用）
- 老固件忽略整个 `emotions` 字段（只见多几帧素材），可安全升级素材

素材包实例（内置绿豆回归包，含生成脚本）：
`assets/branding/bean-emotions.py` 可从基帧自动派生一套绿豆情绪帧
（专注/惊讶/开心/垂泪眼 + 微笑/撇嘴），并写好 `emotions` 表。

## 动作表（actions）

`actions` 让素材包声明**具名动作**（挥手、打哈欠、点头……）。动作名是素材
包内部的组织名，对外触发走 `commands` 命令映射（见「跨角色平台契约」的两级
绑定模型）。
动作有两种形态：**帧表动作**（本节，只动既有层的帧序，不加层、不改坐标——动作
素材就是给某个层多画几帧，或复用五官情绪帧的时序组合）与 **GIF 整帧动作**
（下节，一张整帧动画换创作自由度）。

```json
"actions": {
  "yawn":  { "frame_ms": 200, "loops": 2,
             "tracks": { "eyes": [1, 1, 1, 0], "mouth": [1, 1, 0, 0] } },
  "cheer": { "frame_ms": 150, "loops": 3,
             "tracks": { "eyes": [4, 0, 4, 0], "mouth": [2, 1, 2, 1] } }
}
```

字段说明：

| 字段 | 必需 | 说明 |
| --- | --- | --- |
| `actions` | 否 | 动作表。缺省时该助手没有任何可触发动作 |
| 动作名 | 是 | 小写字母开头，`[a-z0-9_]`，≤16 字符（与桥接器能力名同一规则） |
| `frame_ms` | 否 | 每步时长，30–2000ms，缺省 120。注意设备动效泵约 90ms 一拍，过短的步长会被量化 |
| `loops` | 否 | 播放遍数 1–10，缺省 1；总时长 = 最长轨道长度 × loops × frame_ms |
| `tracks` | 是 | 层 id → 帧序号数组（0 起）。各轨道独立循环取帧；引用不存在的层或越界帧序号的轨道整个丢弃 |

行为与约束：

- **可用性由素材自己决定**：manifest 没声明的动作、轨道全被修剪掉的动作，
  天然不可用。设备收到不可用动作的触发指令时**静默忽略**（不报错、不动），
  这就是行动系统的降级语义
- 动作播放期间，被引用层的帧由动作接管（优先于 blink/talk/emotions）；
  播完自动回到情绪/状态驱动。换装（切助手/换版本）立即停掉在播动作
- 数量上限：≤16 个动作，每个动作 ≤8 条轨道，每条轨道 ≤32 步
- 被 `actions` 引用的层不参与呼吸帧烘焙（烘焙会释放原帧），烘焙预算不受影响
- 桥接器校验 actions 表（≤16 动作 / 每动作 ≤8 轨道 / 每轨道 ≤32 步、引用的
  层与帧序号必须存在），坏表拒收整个素材包；设备端解析时坏条目只影响自己

### GIF 整帧动作（第二种动作形态）

帧表动作只能复用已有层的帧序，做不了自由运动。GIF 动作补上这条路线：动作 =
一张整帧动画 + 播放时长，设备起全画布覆盖层流式解码播放（固件内置精简
GIF87a/89a 解码器，逐帧 LZW 解码在 LVGL 线程跑，单帧约几毫秒）。

```json
"actions": {
  "dance": { "gif": "dance.gif", "ms": 3000 }
}
```

| 字段 | 必需 | 说明 |
| --- | --- | --- |
| `gif` | 是 | 包内 GIF 文件名（与 PNG 同名规则：≤16 位 `[A-Za-z0-9_-]` + `.gif`）；同一文件可被多个 GIF 动作引用 |
| `ms` | 否 | 播放时长 100–10000ms，缺省 3000；时长内解完整遍则 rewind 循环续播 |

行为：播放期 GIF 覆盖层盖住全部分层形象（素材层整体隐藏，避免透明区透出双轮廓
鬼影），随容器呼吸/摆动；到 `ms` 撤层回到状态驱动，中途换装/切助手立即中断。
文件缺失或解码失败静默降级（与帧表动作一致，不报错）。同页同时最多播一个 GIF
动作（GIF 播放期间收到帧表动作命令不排队，动作优先级最高但互不叠加的语义不变）。

素材硬约束（桥接器校验，超限拒收）：

- 画布 ≤512×512（桥接器兼容老包；**新素材直接用 240×240**——设备帧槽按 240×240 定容，单帧解码 >225KB 会回退堆分配）、单文件 ≤256KB（计入整包 512KB 预算，建议 ≤150KB）；全彩 PNG >32KB 先 quantize 256 色（P 模式带透明度兼容设备解码，通常再省 3-5 倍）
- 超限时用 made-kit 的 `shrink_gif.py` 压缩（先降色数、不够再均匀砍帧且总时长不变），
  压完重跑 `check_gif.py`
- 帧率 ≤10fps（帧延时 ≥100ms）；逐帧带透明索引、`optimize=False` 导出
  （透明索引是帧间差分与露底色的前提，PIL 优化导出会丢后继帧的透明标志）
- disposal 建议 1/2；`actions` 里每个 GIF 引用必须存在于包内，GIF 也参与
  「目录里的文件必须被引用」检查

生成方式见 made-kit 技能包 sprite 分册「动作素材」一节（生成式 AI 出视频转
GIF / `action_gif.py` 从主图派生 / 外部工具导出），AI 素材的绿/品红幕用
`key_gif.py` 抠透明（生成端背景不透明会盖住整个舞台，抠幕是硬要求），
自检工具 `check_gif.py`。

格式路线取舍（选型记录）：GIF 的 alpha 只有 1-bit（无半透明，软阴影/柔边会出
锯齿，素材规避：扁平色块 + 实色暗面）、256 色上限，但设备端解码器小、每帧几毫秒，
是下发格式的正确选择。动态 WebP（8-bit alpha）是创作侧更好的中间格式
（生图引擎 WebP 关键帧 → ffmpeg 转 GIF），但 VP8/VP8L 解码器体积与每帧 CPU
都高出一个量级，不做设备端解码。QOI 解码极简且全 alpha，是将来「帧序列动作」
的候选，但静态体积比 PNG 大数倍（挤爆整包预算）且解码后驻留内存与格式无关，
分层 PNG 与整帧格式维持现状。

### 设备↔桥接器的动作协议

两级绑定模型：**事件→命令**由增强插件（made-kit）的配置定义，命令是纯编号
`A1`–`A9`（不定语义）；**命令→素材**由素材 manifest 的 `commands` 表定义。
设备把可用命令作为 `action.<名>` 条目**并入命令清单**上报
（`POST /device/capabilities`，与 ui.tap 等静态命令同表；无素材/无动作时清单里
没有动作条目）。素材切换导致名单变化时，设备重报整张清单：

- 素材声明了 `commands` 表：清单上报**命令号**（`action.A1`…），插件/事件
  绑定只发命令号；设备收到后查 `commands` 映射到真实动作再播。
  声明了 `commands` 的素材**只认命令号**，语义名直发不再生效
- 老素材（无 `commands` 表）：清单上报动作语义名（`action.fall`…），
  直发照常生效——**弃用中**，仅为向后兼容保留，新素材一律写 `commands`
- 桥接器从清单派生名单并暴露：`GET /api/state` 的 `deviceActions` 字段，以及插件
  facade 的 `deviceActions(deviceId)`（语义随上报名单：新素材是命令号，老素材是语义名）
- 触发：直接发命令。桥接侧
  `POST /api/board/commands {"deviceId", "name": "action.A1"}`，
  插件用 `bridge.send(deviceId, { name: 'action.A1' })`。
  旧形式 `action.play` + field `action` 仍兼容（设备端两种都认）
- 名单是**参考不是闸门**：桥接器不校验动作命令在派生名单里，
  设备端自己兜底降级

### 各状态素材建议

| 状态 | 眼睛 | 嘴 |
| --- | --- | --- |
| idle | 睁眼（index 0），按 `blink` 节律眨眼 | 第一帧 |
| talking | 睁眼 | 循环全部嘴帧 |
| running | 半闭/专注眼 | 循环（跟随 talk）或固定 |
| waiting | 惊讶（小瞳孔） | 张嘴/固定 |
| offline | 闭眼（当睡觉） | 第一帧 |
| success | 开心眼（^ ^） | 微笑 |
| sad | 垂眼/泪 | 撇嘴 |

## 跨角色平台契约（素材作者 × 插件作者）：两级绑定

素材包与触发插件往往不是同一批人做的。平台把对接拆成两级，两侧互不见面：

```
板侧事件（touch.double / motion.shake…）
   │  ① 事件→命令：增强插件（made-kit）的配置管，每条绑定可开关
   ▼
抽象命令号 A1–A9（纯编号，不定语义）
   │  ② 命令→素材：素材作者在 manifest 的 commands 表里定义
   ▼
真实动作（actions 表里的具名动作，播什么素材作者说了算）
```

- **① 事件→命令（插件侧，已内置）**：made-kit 技能包统一承接板侧事件绑定
  （双击→A1、摇一摇→A2，配置里每条可单独开关）。插件作者写新的触发插件时
  只发命令号 `{ name: 'action.A1' }`，不接触任何语义
- **② 命令→素材（素材侧）**：素材作者在 manifest 里写 `commands` 表，决定
  命令号播自己声明的哪个动作。素材没映射某命令号时，该命令触发被设备
  **静默忽略**（不报错、不动）——降级是硬语义，插件作者可以放心触发
- 命令号是**纯编号**：A1–A9 不携带任何约定含义，同一个号在不同助手上播
  什么完全由各自的素材决定（雪球的 A1 是跌倒，猫咪的 A1 可以是跳舞）

### commands 表（素材作者写）

```json
"actions":  { "fall": { "gif": "fall.gif", "ms": 2000 } },
"commands": { "A1": "fall", "A2": "fall" }
```

| 字段 | 必需 | 说明 |
| --- | --- | --- |
| `commands` | 否 | 命令映射。缺省时该素材走老语义名直发（弃用中） |
| 命令号 | 是 | 固定槽位 `A1`–`A9`，纯编号不定语义 |
| 映射值 | 是 | 必须是 `actions` 表已声明的动作名；多个命令号可指向同一动作 |

声明了 `commands` 的素材只认命令号；上限 9 个槽位。桥接器校验本表
（命令号格式 / 映射目标必须存在），坏表拒收整个素材包。

### 动作组织名推荐（actions 表内部用）

`actions` 的动作名现在只是**素材包内部的组织名**（commands 映射指向它），
不再跨包暴露。跨包协作不再依赖动作名，但包内多动作仍建议语义英文小写
（可读性好）；自由扩展允许：

| 动作名 | 语义 | 建议形态 |
| --- | --- | --- |
| `fall` | 跌倒（被碰倒/摔倒的喜剧反应） | 帧表（body 2 帧循环）或 GIF |
| `dance` | 跳舞 | GIF 整帧动画 |
| `yawn` | 打哈欠 | 帧表（eyes+mouth 组合） |
| `cheer` | 欢呼加油 | 帧表 |
| `nod` | 点头 | 帧表（body 轻微上下） |
| `wave` | 挥手 | 帧表或 GIF |
| `sleep` | 睡觉（配 offline 情绪） | 帧表（eyes 闭眼 + body 塌下） |

### 素材作者侧规范

1. 声明 `commands` 表决定命令号播什么（见上节）；没声明的素材只有老式
   语义名直发可用（弃用中）
2. 动作组织名用语义英文小写（见上表），不用拼音；动作帧文件命名建议
   `<层>-<动作>-<序>.png`（如 `body-fall-a.png`）、GIF 动作 `<动作>.gif`——
   文件名与动作名都只是包内部组织，不参与跨包契约
3. 没画完的动作不要留半截 `actions` 条目：轨道引用不存在的层会被整条
   修剪，动作可能因此不可用而不自知——本地用 `npx made-market validate`
   校验
4. 动作时长体感：帧表动作 0.5–2s、GIF 动作 2–5s 为宜；触发方会假设
   动作「短平快」，超长动作会被后续状态切换打断

### 插件作者侧规范

1. **优先复用 made-kit 的事件绑定**：双击/摇一摇→命令号开箱即用，
   在 made-kit 配置里开关即可，不用写插件
2. 确需新触发形态时，板侧事件是固件标准事件，直接订阅：`touch.double`（助手页
   双击）、`motion.shake`（摇一摇，需板子带 IMU）、`boot.click` / `boot.double` /
   `boot.triple`（BOOT 键单击/双击/三击）。事件 fields 携带
   `agent`（当前精灵页助手 ID），插件据此做生效范围过滤
3. 触发只发命令号：`bridge.send(deviceId, { name: 'action.A1' })`；
   board-plugin 的 `commands` 数组声明用到的 `action.<号>`
4. **不要假设命令一定有效**：素材没映射该命令号时设备静默忽略，
   插件不应该把它当错误上报或重试轰炸（设备端有冷却与限流）
5. 参考实现：made-kit 技能包（`~/made/plugins/made-kit/`，事件绑定 +
   开关配置；早期的 shake_fall 独立插件已下架，功能并入 made-kit）

## 图片格式与尺寸约束

- **PNG，RGBA**。透明背景；每层一张，不要把多层拼进一张图
- 文件名 `1–16` 个字母/数字/下划线/连字符 + `.png`（**≤20 字符**）：固件把素材平铺
  缓存在 SPIFFS，缓存对象名 = `/` + 前缀(`s` + 6 位哈希 + `_`) + 文件名 + 临时后缀(`~`)，
  而 SPIFFS 对象名硬上限是 31 字节，所以文件名上限必须 ≤20（留 1 字节余量），超长会被板侧拒绝
- 单层 PNG 建议 **≤64KB**、整包 `sprite/` **≤256KB 单文件 / 512KB 整包**（超过会拉取缓慢，圆屏上也看不清细节）
- 尺寸：层图片不必等于画布，按需裁小以省体积；固件按 manifest 的 `x`/`y` 摆放
- 颜色越少越好：小色块层（眼睛、嘴）用 PNG-8 调色板可以小一个数量级
- 固件下载后把 PNG 一次性解码成 RGB565A8 驻留 PSRAM（3 字节/像素；仅当前助手的素材在内存，
  切助手时重新解码）；SPIFFS 存原始 PNG 用于重启加速与免重拉

### 素材压缩规范（生成后必跑）

**所有素材图（PNG/GIF）在生成或修改后必须过一遍 `tools/compress_images.py`**，
生成脚本（`assets/branding/bean-emotions.py`、`split-bean-layers.py`）已在尾部自动调用；
手工改素材时手动跑：

```bash
python3 tools/compress_images.py <文件或目录...>              # extreme（默认）
python3 tools/compress_images.py <路径> --max-dim 240         # 同时纠正超规格素材
python3 tools/compress_images.py <路径> --preset balanced     # 好板子：画质优先
```

四档梯度按**目标板子能力**选，同一套素材源发布到不同板子跑不同档：

| 档位 | PNG | GIF | 适用 |
| --- | --- | --- | --- |
| `extreme`（默认） | 64 色 | 64 色无抖动 | 现役 240 圆屏小内存板 |
| `high` | 128 色 | 128 色无抖动 | 中档板，渐变更顺滑 |
| `balanced` | 256 色 | 256 色 + bayer 抖动 | 好板子，画质优先、体积最大 |
| `lossless` | 仅重 deflate | 不重编码（GIF 无真无损） | 存档/归档 |

- PNG：**分层 alpha 保真量化**（`quantize_rgba_safe`）+ 最大 deflate；JPEG 按档重
  编码（q70~95）。PNG 64 色在 RGB565 屏深下与 256 色不可分辨（屏深才是精度瓶颈），
  实测全包再省 ~27%；特征层主色完整保留，情绪派生管线安全。
  ⚠️ 不能用 PIL 自带 `quantize(FASTOCTREE)` 直接压低色数 RGBA：调色板条目不足时
  会把纯透明 `(0,0,0,0)` 与半透明深色合并（实测 64 色把透明背景顶成 `(0,0,0,174)`，
  真机显示暗色矩形）。分层方案：alpha<60 → 透明项（固件契约）；软边
  （60≤a<250，约 10%）四元组小聚类；不透明主体 RGB 量化占剩余预算；调色板每项
  独立 alpha（tRNS per-entry，固件 `png_set_expand` 链支持）
  GIF 走 ffmpeg palettegen/paletteuse 差分调色板重编码（保透明与逐帧延时）。
  检测到 pngquant/gifsicle 时自动改用（质量更好），无则 PIL/ffmpeg 兜底
- GIF 体积的最大来源是**抖动噪声**——噪声让帧间差分矩形失效、LZW 也压不住；
  64c 无抖动在 240 圆屏与 256c+bayer 基本不可分辨（软渐变轻微色带）。GIF 格式的
  硬天花板就在这；再往下只有「动作帧序列覆盖层」的结构性方案（需固件扩展，
  见「动作表」）
- **幂等**：收益 <3% 的文件自动跳过（`--min-save`），重复跑不会反复劣化画质
- **安全约束**：`--max-dim` 缩尺寸属于运行时隐患修复（超规格帧的解码画布按像素数吃
  PSRAM，见下方警示），即使文件变大也强制落盘
- 有改动时自动递增所在目录 `manifest.json` 的 `version`（每目录每轮只 +1）
- 原文件备份在 `/tmp/img-compress-backup/<时间戳>/`
- **画布一致性硬约束**：层图片与 GIF 的实际像素尺寸不得超过 manifest `canvas`（GIF 的
  解码画布 = GIF 自身尺寸 × 4B/px ARGB，与 PNG 层的 3B/px RGB565A8 槽位无关）。历史上
  `fall.gif`（360 装在 240 包）与 `dance.gif` 均因此埋雷；缩 GIF 用 `--max-dim 240`

## 状态与动效语义

固件按助手当前状态驱动各层，素材只是提供「原材料」：

| 助手状态 | 固件动效 | 消费的素材 |
| --- | --- | --- |
| 待机 | 呼吸缩放 + 左右轻摆 + 定时眨眼 | 全部层；`emotions.idle` |
| 录音/说话中 | 轻微跳动 + 嘴层循环 | `talk: true` 的层；`emotions.talking` |
| 任务运行中 | 呼吸加快 | 全部层；`emotions.running` |
| 任务待确认 | `alert: pulse` 层脉冲 + 身后光圈 | `alert` 层；`emotions.waiting` |
| 任务刚完成 | 瞬时（~3.5s）轻快摇摆 | `emotions.success` |
| 任务刚失败/取消 | 瞬时（~3.5s）蔫缓摇摆 | `emotions.sad` |
| 断连/离线 | 动效停止，静态显示 | 全部层；`emotions.offline` |

`emotions` 之外的层固定显示第一帧（单帧层）或首帧（多帧层）。

## 桥接器与传输协议

- 桥接器扫描已安装 agent 包的 `sprite/`，在 `/device/config` 对应 provider 上暴露：
  `sprite: { version, files: [{ file, bytes, sha256 }] }`（清单很小，走现有通道）
- 素材本体走**分片拉取端点**：`GET /device/sprite/<provider>/<file>?offset=N&len=L`，
  单片 **4KB**（桥接器 `SPRITE_DEVICE_CHUNK`，请求更大的 `len` 也会被截到 4KB——
  大分片在 USB 帧转发上实测丢帧，勿调大），远低于 USB 帧转发的 256KB 上限，三种接入
  方式（局域网 / USB 直连 / 接收器转发）行为一致
- 码得按 `version` 决定是否重拉；逐片下载校验 `sha256`，断点续传，完整落盘后才切换显示
- 拉取失败不阻塞助手页：沿用旧缓存或兜底形象，下轮再试

## 打包与上架

1. `npx made-market init <目录> --kind agent` 生成脚手架，自带 `sprite/` 目录与
   带注释的 manifest 占位
2. 替换占位 PNG，按上文约束检查体积
3. 改素材后**必须递增 `manifest.json` 的 `version`**，否则已安装设备不会更新
4. `npx made-market validate <目录>` 通过后 `publish`
