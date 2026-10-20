#!/usr/bin/env python3
"""把绿豆立绘拆成固件内置 sprite 的分层素材（docs/SPRITES.md 格式的第一个实例）。

输入 assets/branding/bean-cut.png（方形透明底抠图），输出：
  - bean-body.png           112x112，眼/嘴被修掉的身体层（头顶缺口折痕保留）
  - bean-eyes-open.png / bean-eyes-closed.png   双眼合一层，两帧同尺寸
  - bean-mouth-closed.png / bean-mouth-open.png 嘴层，两帧同尺寸
  - 固件 C 资产 img_sprite_bean.c（ARGB8888，与 img_app_vibe 同格式）
  - stdout 打印 112 基准坐标（供固件内置 manifest 使用）

检测：轮廓内缩后按暗度做连通域——两个最大的暗块是眼睛，其下最大的暗块是嘴，
其余暗色细节（头顶缺口折痕等）归身体层。层素材按缩放后的真实位置/尺寸输出，
不居中补边，保证与身体层逐像素对齐。
"""
from collections import deque
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent
SRC = ROOT / 'bean-cut.png'
OUT_C = ROOT.parent.parent / 'made' / 'components' / 'brookesia_app_vibe' / 'assets' / 'img_sprite_bean.c'
OUT_H = ROOT.parent.parent / 'made' / 'components' / 'brookesia_app_vibe' / 'assets' / 'img_sprite_bean.h'
BASIS = 112

im = Image.open(SRC).convert('RGBA')
w, h = im.size
px = im.load()
interior = im.getchannel('A').filter(ImageFilter.MinFilter(31))
ipx = interior.load()

def is_dark(x, y):
    r, g, b, a = px[x, y]
    return a > 128 and r < 80 and g < 100 and b < 80 and ipx[x, y] > 200

pts = {(x, y) for y in range(h) for x in range(w) if is_dark(x, y)}
blobs = []
while pts:
    seed = next(iter(pts))
    q, blob = [seed], []
    pts.discard(seed)
    while q:
        c = q.pop()
        blob.append(c)
        x, y = c
        for n in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if n in pts:
                pts.discard(n)
                q.append(n)
    blobs.append(blob)
blobs.sort(key=len, reverse=True)
assert len(blobs) >= 3, f'暗块太少：{len(blobs)}'

def bbox(points, pad=0):
    xs = [p[0] for p in points]
    ys = [p[1] for p in points]
    return (max(0, min(xs) - pad), max(0, min(ys) - pad),
            min(w, max(xs) + pad), min(h, max(ys) + pad))

def median_color(points):
    ch = [[px[x, y][i] for x, y in points] for i in range(3)]
    m = len(points) // 2
    return tuple(sorted(c)[m] for c in ch)

eyes = blobs[0], blobs[1]
assert all(max(p[1] for p in b) < h * 0.55 for b in eyes), '眼睛不在上半部，检测可疑'
below = [b for b in blobs[2:] if sum(p[1] for p in b) / len(b) > max(p[1] for p in eyes[0] + eyes[1]) * 1.15]
assert below, '没找到嘴'
mouth = max(below, key=len)
eye_pad = (bbox(eyes[0])[2] - bbox(eyes[0])[0]) // 8
mouth_pad = max(4, (bbox(mouth)[2] - bbox(mouth)[0]) // 6)
eyes_box = bbox(eyes[0] + eyes[1], eye_pad)
mouth_box = bbox(mouth, mouth_pad)
eye_color = median_color(eyes[0])
mouth_color = median_color(mouth)
print('eyes bbox', eyes_box, 'eye color', eye_color)
print('mouth bbox', mouth_box, 'mouth color', mouth_color)

def luminance(p):
    return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]

face_samples = [px[max(0, eyes_box[0] - 20), eyes_box[1] + 20], px[min(w - 1, eyes_box[2] + 20), eyes_box[3] - 20]]
face_l = sum(luminance(p) for p in face_samples) / len(face_samples)

# 特征掩膜：blob 像素膨胀 4px，框内亮度低于脸的 AA 边缘一并算入。
def feature_mask(box, blobs_):
    x0, y0, x1, y1 = box
    mask = Image.new('L', (x1 - x0, y1 - y0), 0)
    mpx = mask.load()
    for blob in blobs_:
        for x, y in blob:
            if x0 <= x < x1 and y0 <= y < y1:
                for dy in range(-4, 5):
                    for dx in range(-4, 5):
                        mx, my = x - x0 + dx, y - y0 + dy
                        if 0 <= mx < mask.width and 0 <= my < mask.height:
                            mpx[mx, my] = 255
    return mask

def inpaint(box, blobs_):
    x0, y0, x1, y1 = box
    mask = feature_mask(box, blobs_)
    mpx = mask.load()
    for y in range(max(0, y0 - 4), min(h, y1 + 4)):
        for x in range(max(0, x0 - 4), min(w, x1 + 4)):
            in_mask = x0 <= x < x1 and y0 <= y < y1 and mpx[x - x0, y - y0] > 60
            if not in_mask:
                continue
            fill = None
            for dx in range(1, w):
                for cand in (x - dx, x + dx):
                    if 0 <= cand < w and px[cand, y][3] > 128 and luminance(px[cand, y]) > face_l - 90:
                        fill = px[cand, y][:3]
                        break
                if fill:
                    break
            if fill:
                bpx[x, y] = (*fill, 255)

body = im.copy()
bpx = body.load()
inpaint(eyes_box, eyes)
inpaint(mouth_box, [mouth])

# ---- 眼/嘴层：掩膜内亮度差 → alpha 抠色（保证 AA 边缘平滑） ----------------
def blank(box):
    return Image.new('RGBA', (box[2] - box[0], box[3] - box[1]), (0, 0, 0, 0))

def cutout(box, blobs_, blob_color, drawer=None, keep_source=True):
    layer = blank(box) if not keep_source else blank(box)
    if keep_source:
        src = im.crop(box)
        spx, lpx = src.load(), layer.load()
        mask = feature_mask(box, blobs_)
        mpx = mask.load()
        lo = luminance(blob_color)
        for y in range(layer.height):
            for x in range(layer.width):
                if mpx[x, y] <= 60:
                    continue
                p = spx[x, y]
                if p[3] < 40:
                    continue
                a = (face_l - luminance(p)) / max(1.0, face_l - lo)
                alpha = max(0, min(255, int(a * 255)))
                if alpha > 8:
                    lpx[x, y] = (*blob_color, alpha)
    if drawer:
        drawer(layer)
    return layer

def closed_eyes(layer):
    """闭眼：每只眼下缘一条下弯弧线，宽度与眼簇同宽。"""
    d = ImageDraw.Draw(layer)
    for blob in eyes:
        bx0, by0, bx1, by1 = bbox(blob)
        x0, y0 = bx0 - eyes_box[0], by0 - eyes_box[1]
        x1, y1 = bx1 - eyes_box[0], by1 - eyes_box[1]
        width = max(3, (x1 - x0) // 7)
        d.line([(x0 + width, y0 + (y1 - y0) // 4), ((x0 + x1) // 2, y1 - width // 2),
                (x1 - width, y0 + (y1 - y0) // 4)],
               fill=(*eye_color, 255), width=width, joint='curve')

def open_mouth(layer):
    """张嘴：原嘴形纵向拉伸的实心椭圆，中心与闭嘴一致。"""
    mw, mh = layer.width, layer.height
    d = ImageDraw.Draw(layer)
    cx, cy = mw / 2, mh / 2
    d.ellipse([mw * 0.08, cy - mh * 1.05, mw * 0.92, cy + mh * 1.05],
              fill=(*mouth_color, 255))

eyes_open = cutout(eyes_box, eyes, eye_color)
eyes_closed = cutout(eyes_box, eyes, eye_color, closed_eyes, keep_source=False)
mouth_closed = cutout(mouth_box, [mouth], mouth_color)
mouth_open = cutout(mouth_box, [mouth], mouth_color, open_mouth, keep_source=False)

# ---- 缩到 112 基准：身体层整图 contain；特征层按真实位置缩放（不补边） ----
def resize_scaled(img):
    k = BASIS / max(w, h)
    return img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))),
                      Image.Resampling.LANCZOS), k

body112, _ = resize_scaled(body)          # 方图 → 112x112
eyes_open112, _ = resize_scaled(eyes_open)
eyes_closed112, _ = resize_scaled(eyes_closed)
mouth_closed112, _ = resize_scaled(mouth_closed)
mouth_open112, _ = resize_scaled(mouth_open)

for name, img in [('bean-body.png', body112), ('bean-eyes-open.png', eyes_open112),
                  ('bean-eyes-closed.png', eyes_closed112), ('bean-mouth-closed.png', mouth_closed112),
                  ('bean-mouth-open.png', mouth_open112)]:
    img.save(ROOT / name)
    print(name, img.size, f'{len(img.tobytes("raw", "BGRA"))} bytes')

# 素材压缩规范（docs/SPRITES.md）：生成的帧统一过激进压缩（幂等，已压过会跳过）。
import sys as _sys
_sys.path.insert(0, str(ROOT.parent.parent / 'tools'))
from compress_images import compress_file as _compress_file
for name, _img in [('bean-body.png', None), ('bean-eyes-open.png', None),
                   ('bean-eyes-closed.png', None), ('bean-mouth-closed.png', None),
                   ('bean-mouth-open.png', None)]:
    _compress_file(str(ROOT / name))

def scale_box(box):
    k = BASIS / max(w, h)
    return tuple(round(v * k) for v in box)

print('LAYER_POS eyes', scale_box(eyes_box), eyes_open112.size)
print('LAYER_POS mouth', scale_box(mouth_box), mouth_closed112.size)

# 预览图：按真实位置叠 body + 闭眼 + 闭嘴，人工核对对齐用
preview = Image.new('RGBA', (BASIS, BASIS), (235, 235, 235, 255))
def paste(canvas, img, box):
    k = BASIS / max(w, h)
    canvas.alpha_composite(img, (round(box[0] * k), round(box[1] * k)))
preview.alpha_composite(body112)
paste(preview, eyes_open112, eyes_box)
paste(preview, mouth_closed112, mouth_box)
preview.save(ROOT / 'bean-layers-preview.png')
preview_closed = Image.new('RGBA', (BASIS, BASIS), (235, 235, 235, 255))
preview_closed.alpha_composite(body112)
paste(preview_closed, eyes_closed112, eyes_box)
paste(preview_closed, mouth_open112, mouth_box)
preview_closed.save(ROOT / 'bean-layers-preview-closed.png')

# ---- 固件 C 资产 ------------------------------------------------------------
def c_descriptor(var, img):
    pixels = img.tobytes('raw', 'BGRA')
    rows = ['    ' + ', '.join(f'0x{b:02x}' for b in pixels[i:i + 24]) + ','
            for i in range(0, len(pixels), 24)]
    return ('\n'.join(rows),
            f'const lv_image_dsc_t {var} = {{\n'
            f'    .header.cf = LV_COLOR_FORMAT_ARGB8888,\n'
            f'    .header.magic = LV_IMAGE_HEADER_MAGIC,\n'
            f'    .header.w = {img.width},\n'
            f'    .header.h = {img.height},\n'
            f'    .header.stride = {img.width} * 4,\n'
            f'    .data_size = sizeof({var}_map),\n'
            f'    .data = {var}_map,\n'
            f'}};\n')

parts_out, desc_out = [], []
for var, img in [('img_bean_body', body112), ('img_bean_eyes_open', eyes_open112),
                 ('img_bean_eyes_closed', eyes_closed112), ('img_bean_mouth_closed', mouth_closed112),
                 ('img_bean_mouth_open', mouth_open112)]:
    maps, desc = c_descriptor(var, img)
    parts_out.append(f'const uint8_t {var}_map[] = {{\n{maps}\n}};\n')
    desc_out.append(desc)

eyes_pos = scale_box(eyes_box)
mouth_pos = scale_box(mouth_box)
OUT_H.write_text(
    '// SPDX-License-Identifier: Apache-2.0\n'
    '// Generated by assets/branding/split-bean-layers.py. 内置绿豆 sprite 的\n'
    '// 特征层坐标（112 基准，相对完整画布左上角）；固件内置 manifest 使用。\n'
    '#pragma once\n\n'
    f'#define IMG_BEAN_BASIS {BASIS}\n'
    f'#define IMG_BEAN_EYES_X {eyes_pos[0]}\n'
    f'#define IMG_BEAN_EYES_Y {eyes_pos[1]}\n'
    f'#define IMG_BEAN_MOUTH_X {mouth_pos[0]}\n'
    f'#define IMG_BEAN_MOUTH_Y {mouth_pos[1]}\n',
    encoding='utf-8')

OUT_C.write_text(
    '// SPDX-License-Identifier: Apache-2.0\n'
    '// Generated by assets/branding/split-bean-layers.py. 内置绿豆 sprite 的分层\n'
    '// 素材（body / eyes 两帧 / mouth 两帧），与第三方助手走同一条素材管线。\n'
    '#include "lvgl.h"\n\n' + '\n'.join(parts_out) + '\n' + '\n'.join(desc_out),
    encoding='utf-8')
print('wrote', OUT_C)
