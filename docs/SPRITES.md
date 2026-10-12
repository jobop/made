# 助手形象素材（sprite）开发规范

助手页的小精灵形象不由固件内置，而是**随 agent 运行时插件包下发**：包里带一个
`sprite/` 目录（分层 PNG + manifest），桥接器把素材清单暴露给码得，码得按版本号
分片拉取、落盘缓存。换形象 = 换插件包里的素材，**不需要刷固件**。

本文是素材包的格式真源。改动本文时，三处必须同步：本仓库 [插件开放能力清单](../AGENTS.md)
（AGENTS.md）、市场 CLI 脚手架（made-market 仓库 `cli/`，`npx made-market init --kind agent`
生成的模板）、市场网站 [开放能力说明页](https://made-market.link/capabilities.html)。

## 归属与优先级

形象素材是 **agent 的属性**，挂在 agent 运行时插件包上，不进主题插件、不进专家包：

| 优先级 | 来源 | 说明 |
| --- | --- | --- |
| 1 | agent 插件包的 `sprite/` | 本文规范的主体；版本号变化即重新拉取 |
| 2 | 固件内置兜底形象 | 未连接桥接器、包里没带 sprite、或素材拉取失败时显示 |

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
    "breath": { "scale_pct": 103, "period_ms": 3000 },
    "sway_deg": 3,
    "sway_period_ms": 1800
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
| `layers[].blink` | 否 | 眨眼节奏：每 `period_ms` 闭眼 `closed_ms`。只对 `eyes` 层生效 |
| `layers[].talk` | 否 | `true` 时录音/说话状态下循环播 `frames`（嘴型） |
| `layers[].alert` | 否 | `pulse`：待确认任务时该层做透明度脉冲提醒 |
| `motion` | 否 | 整体动效参数；缺省用固件默认值（呼吸 3% / 摆动 3° / 1800ms） |

## 图片格式与尺寸约束

- **PNG，RGBA**。透明背景；每层一张，不要把多层拼进一张图
- 单层 PNG 建议 **≤64KB**、整包 `sprite/` **≤512KB**（超过会拉取缓慢，圆屏上也看不清细节）
- 尺寸：层图片不必等于画布，按需裁小以省体积；固件按 manifest 的 `x`/`y` 摆放
- 颜色越少越好：小色块层（眼睛、嘴）用 PNG-8 调色板可以小一个数量级
- 固件首次下载后把 PNG 解码成 RGB565 落盘缓存（SPIFFS），播放时不再解码；所以
  开发者不用关心解码成本，但**缓存空间有限，整包别铺张**

## 状态与动效语义

固件按助手当前状态驱动各层，素材只是提供「原材料」：

| 助手状态 | 固件动效 | 消费的素材 |
| --- | --- | --- |
| 待机 | 呼吸缩放 + 左右轻摆 + 定时眨眼 | 全部层 |
| 录音/说话中 | 轻微跳动 + 嘴层循环 | `talk: true` 的层 |
| 任务运行中 | 呼吸加快（周期减半） | 全部层 |
| 任务待确认 | `alert: pulse` 层脉冲 + 身后光圈 | `alert` 层 |
| 断连/离线 | 动效停止，静态显示 | 全部层 |

`blink` / `talk` / `alert` 之外的层固定显示第一帧（单帧层）或首帧（多帧层）。

## 桥接器与传输协议

- 桥接器扫描已安装 agent 包的 `sprite/`，在 `/device/config` 对应 provider 上暴露：
  `sprite: { version, files: [{ file, bytes, sha256 }] }`（清单很小，走现有通道）
- 素材本体走**分片拉取端点**：`GET /device/sprite/<provider>/<file>?offset=N&len=L`，
  单次响应 **≤192KB**——远低于 USB 帧转发的 256KB 上限，三种接入方式
  （局域网 / USB 直连 / 接收器转发）行为一致
- 码得按 `version` 决定是否重拉；逐片下载校验 `sha256`，断点续传，完整落盘后才切换显示
- 拉取失败不阻塞助手页：沿用旧缓存或兜底形象，下轮再试

## 打包与上架

1. `npx made-market init <目录> --kind agent` 生成脚手架，自带 `sprite/` 目录与
   带注释的 manifest 占位
2. 替换占位 PNG，按上文约束检查体积
3. 改素材后**必须递增 `manifest.json` 的 `version`**，否则已安装设备不会更新
4. `npx made-market validate <目录>` 通过后 `publish`
