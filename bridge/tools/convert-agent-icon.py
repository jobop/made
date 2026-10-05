#!/usr/bin/env python3
"""Convert a local image to the bounded bitmap format used by Vibe Bridge.

Development tool only. Runtime bridge and firmware do not depend on Pillow.
"""
import argparse
import base64
import json
import re
import sys
from pathlib import Path


def convert_icon(source, size=48, background='#263B59', accent='#B7DDF3'):
    try:
        from PIL import Image, ImageOps
    except ImportError as exc:
        raise ValueError('需要 Pillow：请在开发环境执行 python3 -m pip install Pillow') from exc
    if not 1 <= size <= 48:
        raise ValueError('size 必须在 1–48 之间')
    for name, value in [('background', background), ('accent', accent)]:
        if not re.fullmatch(r'#[0-9a-fA-F]{6}', value):
            raise ValueError(f'{name} 必须为 #RRGGBB')
    if Path(source).stat().st_size > 16 * 1024 * 1024:
        raise ValueError('原始图片请限制在 16 MB 内')
    with Image.open(source) as original:
        if original.width * original.height > 16_777_216:
            raise ValueError('原始图片尺寸过大，请先缩小')
        # EXIF orientation applies before the image is fit into a square.
        rgba = ImageOps.exif_transpose(original).convert('RGBA')
        rgba.thumbnail((size, size), Image.Resampling.LANCZOS)
        canvas = Image.new('RGBA', (size, size), (0, 0, 0, 0))
        canvas.alpha_composite(rgba, ((size - rgba.width) // 2, (size - rgba.height) // 2))
        quantized = canvas.quantize(colors=16, method=Image.Quantize.FASTOCTREE).convert('RGBA')
        palette = []
        indices = []
        lookup = {}
        for color in quantized.getdata():
            # Invisible pixels have no meaningful RGB and use one palette slot.
            if color[3] == 0:
                color = (0, 0, 0, 0)
            if color not in lookup:
                lookup[color] = len(palette)
                palette.append(''.join(f'{component:02X}' for component in color))
            indices.append(lookup[color])
    if len(palette) > 16:
        raise ValueError('图标调色板超过 16 色')
    packed = bytearray((len(indices) + 1) // 2)
    for index, color in enumerate(indices):
        packed[index // 2] |= color << (4 if index % 2 == 0 else 0)
    return {'format': 'indexed4', 'width': size, 'height': size, 'palette': palette,
            'data': base64.b64encode(packed).decode('ascii'),
            'background': background.upper(), 'accent': accent.upper()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='PNG、JPEG、WebP 等本机图片')
    parser.add_argument('output', type=Path, help='输出 icon.json，供助手插件导入')
    parser.add_argument('--size', type=int, default=48, help='边长 1–48，默认 48')
    parser.add_argument('--background', default='#263B59')
    parser.add_argument('--accent', default='#B7DDF3')
    parser.add_argument('--force', action='store_true', help='覆盖已有输出文件')
    args = parser.parse_args()
    try:
        icon = convert_icon(args.source, args.size, args.background, args.accent)
        with args.output.open('w' if args.force else 'x', encoding='utf-8') as output:
            json.dump(icon, output, ensure_ascii=False, separators=(',', ':'))
            output.write('\n')
    except (ValueError, OSError) as error:
        parser.exit(1, f'{error}\n')
    print(f'已生成 {args.output}: {args.size}×{args.size}, {len(icon["palette"])} 色', file=sys.stderr)


if __name__ == '__main__':
    main()
