#!/usr/bin/env python3
"""从 imagegen 生成的棋盘格假透明图里抠出绿豆。

背景 = 与图像边缘连通的低饱和灰白像素（棋盘格 + 水印），
豆身绿色饱和度高，泛洪不会漏入； cream 高光在豆内部不与边缘连通。
输出裁剪到豆 bbox（含 2% padding）的 PNG 与眼位比例。
"""
from collections import deque
from pathlib import Path

from PIL import Image

SRC = Path('/Users/myidd007/My project/made/generated-images/Edit_this_exact_app_icon_artwo_2026-10-12T15-42-38.png')
OUT = Path('/Users/myidd007/My project/made/assets/branding/bean-cut.png')

im = Image.open(SRC).convert('RGBA')
w, h = im.size
px = im.load()


def background(x, y):
    r, g, b, _ = px[x, y]
    return max(r, g, b) - min(r, g, b) < 22


mask = bytearray(w * h)
queue = deque()
for x in range(w):
    for y in (0, h - 1):
        if background(x, y) and not mask[y * w + x]:
            mask[y * w + x] = 1
            queue.append((x, y))
for y in range(h):
    for x in (0, w - 1):
        if background(x, y) and not mask[y * w + x]:
            mask[y * w + x] = 1
            queue.append((x, y))
while queue:
    x, y = queue.popleft()
    for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
        if 0 <= nx < w and 0 <= ny < h and not mask[ny * w + nx] and background(nx, ny):
            mask[ny * w + nx] = 1
            queue.append((nx, ny))

for y in range(h):
    for x in range(w):
        if mask[y * w + x]:
            r, g, b, a = px[x, y]
            px[x, y] = (r, g, b, 0)

bbox = im.getbbox()
pad = int((bbox[2] - bbox[0]) * 0.02)
box = (max(0, bbox[0] - pad), max(0, bbox[1] - pad),
       min(w, bbox[2] + pad), min(h, bbox[3] + pad))
cut = im.crop(box)
cut.save(OUT)
bw, bh = cut.size
cp = cut.load()

eye_pixels = [[], []]
dark = []
for y in range(bh):
    for x in range(bw):
        r, g, b, a = cp[x, y]
        if a > 200 and r < 70 and g < 90 and b < 70:
            dark.append((x, y))
cx = bw / 2
for x, y in dark:
    if y < bh * 0.45:
        eye_pixels[0 if x < cx else 1].append((x, y))


def centroid(points):
    n = len(points)
    return (sum(p[0] for p in points) / n, sum(p[1] for p in points) / n)


left = centroid(eye_pixels[0])
right = centroid(eye_pixels[1])
print(f'bbox_px={bw}x{bh}')
print(f'eye_left_frac=({left[0] / bw:.4f},{left[1] / bh:.4f})')
print(f'eye_right_frac=({right[0] / bw:.4f},{right[1] / bh:.4f})')
