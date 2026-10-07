# 内置主题目录

跟代码一起发布的内置主题，每个子目录 = 一个主题。码得在「设置 → 主题设置」打开时会实时列出，点选即应用（通过当前连接拉取数据，重启生效）。

## 与用户目录的关系

桥接器按下面的顺序扫描，**同名时后者覆盖前者**：

1. `bridge/builtin/themes/` —— 本目录，随代码升级
2. `~/made/themes/` —— 用户目录，主题市场装的主题都在这里

所以内置主题不能从工作台删除（删除只作用于 `~/made/themes/`）。想覆盖某个内置主题，就在 `~/made/themes/` 放一个同名目录，它的 `theme.json` 与资产会顶掉内置的那份。

用户目录可用 `MADE_HOME` 环境变量或配置里的 `madeHome` 改成别的路径。

## 主题包结构

```
themes/<名字>/
├── theme.json   必需：标题、调色板、可选资产声明
├── chime.wav    可选：提示音（新消息铃声）
├── icon.bin     可选：锁屏图标，raw RGB565 小端
└── bg.bin       可选：锁屏背景图，raw RGB565 小端
```

## theme.json 完整示例

```json
{
  "title": "日落",
  "colors": {
    "accent": "E07840",
    "btn_normal": "5C3328",
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
    "lock_bg": "1A0F0C"
  },
  "sound": { "file": "chime.wav" },
  "lock": {
    "icon": "icon.bin", "icon_w": 112, "icon_h": 112,
    "bg": "bg.bin", "bg_w": 240, "bg_h": 320
  }
}
```

## 资产格式说明

- **提示音**：WAV（无压缩 PCM 16bit），采样率 8k~48k，单/双声道均可，
  播放时按文件头自适应；≤256KB（约 8 秒 16k 单声道）
- **图标/背景**：无头的 raw RGB565 小端像素数组（宽×高×2 字节），
  尺寸在 theme.json 里声明；bg 建议与屏幕设计分辨率一致
- **PNG 转 RGB565**：`python3 -c "from PIL import Image; import sys; im = Image.open(sys.argv[1]).convert('RGB'); open(sys.argv[2],'wb').write(b''.join(bytes(((r>>3)<<11)|((g>>2)<<5)|(b>>3)) for r,g,b in im.getdata()))" in.png out.bin`
- 颜色缺省时回落到内置默认；恢复默认主题 = 清空全部主题缓存
- **主题显示名（title）请使用常用汉字或英文**：设备内置中文字库覆盖
  GB2312 常用字，生僻字（如「啵」）会显示为方框
