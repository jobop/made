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

## 最小可用与推荐分层

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
  全画布图层在设备上多付 4 字节/像素解码内存与混合面积（360×360 ARGB8888
  ≈ 506KB/张）；同层多帧必须裁到**并集** bbox，否则共用 `x`/`y` 会错位
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
  "canvas": { "w": 360, "h": 360 },
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
  }
}
```

字段说明：

| 字段 | 必需 | 说明 |
| --- | --- | --- |
| `version` | 是 | 整数，**单调递增**。码得只认版本号，变了才重新拉取 |
| `canvas` | 是 | 画布尺寸。圆屏 360×360；`x`/`y` 是层左上角在画布上的位置 |
| `layers[].id` | 是 | 层标识。`body` / `eyes` / `mouth` / `accessory` 是保留名，其余自定义 |
| `layers[].file` / `frames` | 二选一 | 单帧层用 `file`；动画层用 `frames`（按帧序循环或按状态切换） |
| `layers[].x` / `y` / `z` | 是 | 位置与叠放次序，z 小的在下 |
| `layers[].blink` | 否 | 眨眼节奏：每 `period_ms` 闭眼 `closed_ms`。只对 `eyes` 层生效；闭眼帧约定为 `frames[1]` |
| `layers[].talk` | 否 | `true` 时录音/说话状态下循环播 `frames`（嘴型） |
| `layers[].alert` | 否 | `pulse`：待确认任务时该层做透明度脉冲提醒 |
| `motion` | 否 | 整体动效参数；缺省用固件默认值（呼吸 8% / 摆动 3° / 1800ms） |
| `emotions` | 否 | 情绪表：把助手状态映射到各层的帧（见下节）。缺省时所有状态共用第一套帧（兼容旧素材） |

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

## 图片格式与尺寸约束

- **PNG，RGBA**。透明背景；每层一张，不要把多层拼进一张图
- 文件名 `1–16` 个字母/数字/下划线/连字符 + `.png`（**≤20 字符**）：固件把素材平铺
  缓存在 SPIFFS，缓存对象名 = `/` + 前缀(`s` + 6 位哈希 + `_`) + 文件名 + 临时后缀(`~`)，
  而 SPIFFS 对象名硬上限是 31 字节，所以文件名上限必须 ≤20（留 1 字节余量），超长会被板侧拒绝
- 单层 PNG 建议 **≤64KB**、整包 `sprite/` **≤256KB 单文件 / 512KB 整包**（超过会拉取缓慢，圆屏上也看不清细节）
- 尺寸：层图片不必等于画布，按需裁小以省体积；固件按 manifest 的 `x`/`y` 摆放
- 颜色越少越好：小色块层（眼睛、嘴）用 PNG-8 调色板可以小一个数量级
- 固件下载后把 PNG 一次性解码成 ARGB8888 驻留 PSRAM（仅当前助手的素材在内存，
  切助手时重新解码）；SPIFFS 存原始 PNG 用于重启加速与免重拉

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
