# 主题包目录

每个子目录 = 一个主题。把主题文件夹丢进这里，码得在「设置 → 主题设置 → 从电脑同步主题」即可拉取（设备需与电脑同局域网，Wi-Fi 模式或已配对）。

## 主题包结构

```
themes/<名字>/
├── theme.json   必需：标题与调色板
├── icon.bin     可选：锁屏图标，raw RGB565 小端（与固件 LV_COLOR_16_SWAP 配置一致）
└── bg.bin       可选：锁屏背景，同上
```

## theme.json 示例

```json
{
  "title": "日落",
  "colors": {
    "accent": "E07840",        // 高亮/选中按钮
    "btn_normal": "5C3328",    // 普通按钮
    "btn_cancel": "6B4A38",
    "border": "E0A080",
    "text_primary": "FFF3E8",
    "text_secondary": "E8C4A8",
    "text_hint": "C09070",
    "text_accent": "FFC088",
    "title": "FFE8D0",
    "bg_page": "1F1210",
    "bg_panel": "2A1714",
    "bg_row": "3A201B",
    "danger": "A84040",
    "lock_bg": "1A0F0C"        // 锁屏背景
  },
  "lock": {
    "icon": "icon.bin", "icon_w": 112, "icon_h": 112,
    "bg": "bg.bin", "bg_w": 240, "bg_h": 320
  }
}
```

颜色值缺省时回落到内置默认。字体：v2 预留（需 LVGL 运行时字体加载器）。
