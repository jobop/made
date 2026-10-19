#!/usr/bin/env python3
# 素材页状态图标生成器：16×16 单色透明位图（白色像素 + 抗锯齿 alpha），
# 输出 LVGL ARGB8888 C 数组头文件。运行时用 lv_image recolor 上色，
# 换图标 = 换 src，颜色跟随主题 token。
#
# 用法：python3 gen_status_icons.py  （在仓库任意位置运行，路径内置）
# 修改图形后重跑即可再生成 status_icons.h；预览图写到 /tmp/status-icons-preview.png。

from pathlib import Path
from PIL import Image, ImageDraw

OUT_HEADER = Path(__file__).resolve().parent.parent / "made" / "components" / "brookesia_app_vibe" / "status_icons.h"
OUT_PREVIEW = Path("/tmp/status-icons-preview.png")

SS = 12  # 超采样倍数：16 网格 × 12 = 192 画布，LANCZOS 缩回 16 得抗锯齿 alpha
GRID = 16
STROKE = 1.5  # 线条宽（网格像素）——对应 LV_SYMBOL 16px 的视觉粗细

ICONS = ["idle", "mic", "wave1", "wave2", "ask", "run"]


def new_canvas():
    return Image.new("L", (GRID * SS, GRID * SS), 0)


def draw_idle(d):
    w = STROKE * SS
    d.ellipse([2.5 * SS, 2.5 * SS, 13.5 * SS, 13.5 * SS], outline=255, width=int(round(w)))
    d.ellipse([6.6 * SS, 6.6 * SS, 9.4 * SS, 9.4 * SS], fill=255)


def draw_mic(d):
    w = int(round(STROKE * SS))
    d.rounded_rectangle([6.2 * SS, 1.6 * SS, 9.8 * SS, 8.2 * SS], radius=1.8 * SS,
                        outline=255, width=w)
    d.arc([4.0 * SS, 3.6 * SS, 12.0 * SS, 11.6 * SS], start=0, end=180, fill=255, width=w)
    d.line([8 * SS, 11.6 * SS, 8 * SS, 13.4 * SS], fill=255, width=w)
    d.line([5.2 * SS, 14.2 * SS, 10.8 * SS, 14.2 * SS], fill=255, width=w)


def bars(d, heights):
    w = int(round(1.7 * SS))
    xs = [2.4, 5.2, 8.0, 10.8, 13.6]
    for x, h in zip(xs, heights):
        d.line([x * SS, (8 - h) * SS, x * SS, (8 + h) * SS], fill=255, width=w)


def draw_wave1(d):
    bars(d, [2.2, 3.8, 5.2, 3.8, 2.2])


def draw_wave2(d):
    bars(d, [3.4, 2.0, 3.6, 2.0, 3.4])


def draw_ask(d):
    w = int(round(STROKE * SS))
    d.ellipse([2.2 * SS, 2.2 * SS, 13.8 * SS, 13.8 * SS], outline=255, width=w)
    d.arc([6.0 * SS, 4.6 * SS, 9.8 * SS, 8.4 * SS], start=180, end=30, fill=255, width=w)
    d.line([9.4 * SS, 7.5 * SS, 7.9 * SS, 9.4 * SS], fill=255, width=w)
    d.line([7.9 * SS, 9.4 * SS, 7.9 * SS, 10.0 * SS], fill=255, width=w)
    d.ellipse([7.15 * SS, 11.0 * SS, 8.65 * SS, 12.5 * SS], fill=255)


def draw_run(d):
    w = int(round(1.8 * SS))
    d.arc([2.6 * SS, 2.6 * SS, 13.4 * SS, 13.4 * SS], start=-80, end=170, fill=255, width=w)


DRAWERS = {
    "idle": draw_idle,
    "mic": draw_mic,
    "wave1": draw_wave1,
    "wave2": draw_wave2,
    "ask": draw_ask,
    "run": draw_run,
}


def rasterize(name):
    img = new_canvas()
    DRAWERS[name](ImageDraw.Draw(img))
    small = img.resize((GRID, GRID), Image.LANCZOS)
    return small


def to_c_array(small):
    # LVGL ARGB8888 小端内存序：B,G,R,A。像素全白，颜色交给运行时 recolor。
    out = []
    px = small.load()
    for y in range(GRID):
        row = []
        for x in range(GRID):
            a = px[x, y]
            row.append(f"0x{a:02x}")
        out.append("    " + ", ".join(f"0xff, 0xff, 0xff, {row[i]}" for i in range(0, GRID, 1)) + ",")
    return "\n".join(out)


def main():
    rendered = {name: rasterize(name) for name in ICONS}

    # 预览：8 倍最近邻放大，检查形状
    preview = Image.new("RGBA", (len(ICONS) * 20 * 8, 20 * 8), (228, 225, 218, 255))
    for i, name in enumerate(ICONS):
        big = rendered[name].resize((16 * 8, 16 * 8), Image.NEAREST)
        colored = Image.new("RGBA", big.size, (0, 0, 0, 0))
        cp = colored.load()
        bp = big.load()
        for y in range(big.size[1]):
            for x in range(big.size[0]):
                a = bp[x, y]
                cp[x, y] = (59, 64, 61, a)
        preview.alpha_composite(colored, (i * 20 * 8 + 2 * 8, 2 * 8))
    preview.save(OUT_PREVIEW)

    parts = [
        "#pragma once",
        "// 由 tools/gen_status_icons.py 生成（素材页状态图标，16×16 白色+alpha，",
        "// 运行时 lv_image recolor 上色）。改图形请改生成器重跑，勿手改。",
        "#include \"lvgl.h\"",
        "",
    ]
    for name in ICONS:
        parts.append(f"static const lv_image_dsc_t kStatusIcon{name.capitalize()} = {{")
        parts.append("    .header = {")
        parts.append(f"        .magic = LV_IMAGE_HEADER_MAGIC,")
        parts.append(f"        .cf = LV_COLOR_FORMAT_ARGB8888,")
        parts.append(f"        .w = {GRID},")
        parts.append(f"        .h = {GRID},")
        parts.append(f"        .stride = {GRID * 4},")
        parts.append("    },")
        parts.append(f"    .data_size = {GRID * GRID * 4},")
        parts.append("    .data = (const uint8_t[]){")
        parts.append(to_c_array(rendered[name]))
        parts.append("    },")
        parts.append("};")
        parts.append("")
    OUT_HEADER.write_text("\n".join(parts))
    print(f"written {OUT_HEADER}")
    print(f"preview {OUT_PREVIEW}")


if __name__ == "__main__":
    main()
