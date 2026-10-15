#!/usr/bin/env python3
# bean-emotions.py — 给按 docs/SPRITES.md 规范拆层的绿豆素材包补情绪帧。
#
# 用法：python3 bean-emotions.py <sprite目录>
# （默认 ~/made/plugins/codex/sprite）
#
# 从现有 eyes-open.png / eyes-closed.png / mouth-*.png 的几何（逐眼连通域
# bbox、逐色色值）派生情绪帧，帧尺寸与既有帧完全一致（同层共用 x/y，
# 必须同画布）；manifest 升版本、扩 frames、写 emotions 表。可重复执行
# （幂等：以 eyes-open/eyes-closed/mouth-closed/mouth-open 四个基帧为准
# 重新生成，已有情绪帧直接覆盖）。
#
# 情绪 → 帧映射（与固件 made_sprite Mood 对应）：
#   idle=睁眼    talking=嘴循环    running=专注眼
#   waiting=惊讶眼+张嘴              offline=V 闭眼（睡觉）
#   success=开心眼+微笑嘴           sad=垂眼+眉毛+撇嘴
import json
import math
import os
import sys
from collections import deque

from PIL import Image, ImageDraw

EYE_COLOR = (3, 33, 12, 255)        # 取自 eyes-open 主体色
MOUTH_COLOR = (24, 98, 66, 255)     # 取自 mouth-open 主体色
ALPHA_TH = 60                        # 与固件「软边 alpha<60 视为透明」一致


def blob_bboxes(alpha, thresh=ALPHA_TH):
    """alpha 接近二值化后的连通域 bbox（4 邻接，按 x 排序 = 左眼在前）。"""
    w, h = alpha.size
    px = alpha.load()
    seen = [[False] * w for _ in range(h)]
    boxes = []
    for sy in range(h):
        for sx in range(w):
            if px[sx, sy] < thresh or seen[sy][sx]:
                continue
            q = deque([(sx, sy)])
            seen[sy][sx] = True
            x0, x1, y0, y1 = sx, sx, sy, sy
            while q:
                x, y = q.popleft()
                x0, x1 = min(x0, x), max(x1, x)
                y0, y1 = min(y0, y), max(y1, y)
                for nx, ny in ((x+1, y), (x-1, y), (x, y+1), (x, y-1)):
                    if 0 <= nx < w and 0 <= ny < h and not seen[ny][nx] and px[nx, ny] >= thresh:
                        seen[ny][nx] = True
                        q.append((nx, ny))
            boxes.append((x0, y0, x1, y1))
    boxes.sort()
    return boxes


def stroke(draw, pts, width, color, close=False):
    """圆头折线：PIL line 的 joint='curve' 在大转角会断，用手动圆补。"""
    draw.line(pts, fill=color, width=width, joint='curve')
    r = (width - 1) // 2 + 1
    for (x, y) in (pts[0], pts[-1]):
        draw.ellipse((x - r, y - r, x + r, y + r), fill=color)
    if close:
        x, y = pts[0]
        draw.ellipse((x - r, y - r, x + r, y + r), fill=color)


def arc_points(box, up=True, inset=2):
    """二次贝塞尔：嘴角在两侧、顶点在中部（up=True 顶点朝上=开心 ∩）。"""
    x0, y0, x1, y1 = box
    cx = (x0 + x1) / 2
    p0 = (x0 + inset, y1 - inset if up else y0 + inset)
    p1 = (cx, y0 + inset if up else y1 - inset)
    p2 = (x1 - inset, y1 - inset if up else y0 + inset)
    pts = []
    for i in range(25):
        t = i / 24
        a = (1 - t) ** 2
        b = 2 * t * (1 - t)
        c = t * t
        pts.append((a * p0[0] + b * p1[0] + c * p2[0],
                    a * p0[1] + b * p1[1] + c * p2[1]))
    return pts


TEAR_COLOR = (154, 208, 245, 255)   # 泪珠淡蓝


def gen_eyes(base_eyes, size, eyes):
    """返回情绪名 → 帧尺寸的 RGBA 图。eyes = 逐眼 (x0,y0,x1,y1)。"""
    w, h = size
    out = {}

    # 专注：沿 blob 轮廓裁掉上半（直上眼睑压到瞳孔中线），像盯着手头活计
    img = Image.new('RGBA', size, (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for (x0, y0, x1, y1) in eyes:
        cy = (y0 + y1) // 2
        crop = base_eyes.crop((x0, y0, x1 + 1, y1 + 1))
        amask = crop.split()[3].copy()
        dmask = ImageDraw.Draw(amask)
        dmask.rectangle((0, 0, x1 - x0, cy - y0 - 1), fill=0)   # 上半透明
        img.paste(crop, (x0, y0), amask)
        d.rectangle((x0 + 1, cy - 1, x1 - 1, cy + 2), fill=EYE_COLOR)  # 睑缘线
    out['eyes-focus'] = img

    # 惊讶：瞳孔缩成小点（借用 body 烘焙的眼白当巩膜）。以 blob 质心为中心
    # 缩小重贴（bbox 中心会被右眼的睫毛甩偏），形状跟随原眼。
    img = Image.new('RGBA', size, (0, 0, 0, 0))
    for (x0, y0, x1, y1) in eyes:
        crop = base_eyes.crop((x0, y0, x1 + 1, y1 + 1))
        cw, ch = crop.size
        px = crop.load()
        sx = sy = n = 0
        for yy in range(ch):
            for xx in range(cw):
                if px[xx, yy][3] >= ALPHA_TH:
                    sx += xx; sy += yy; n += 1
        cx, cy = sx / max(n, 1), sy / max(n, 1)
        small = crop.resize((max(1, int(cw * 0.42)), max(1, int(ch * 0.42))))
        img.paste(small, (int(cx - small.width / 2), int(cy - small.height / 2)), small)
    out['eyes-wow'] = img

    # 开心：粗 ^（笑成一条缝，笔画盖住大半眼白才不像愁眉）
    img = Image.new('RGBA', size, (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for (x0, y0, x1, y1) in eyes:
        eh = y1 - y0
        pts = arc_points((x0 + 2, y0 - eh // 6, x1 - 2, y1 + eh // 3), up=True, inset=0)
        stroke(d, pts, width=max(8, int(eh * 0.28)), color=EYE_COLOR)
    out['eyes-happy'] = img

    # 委屈：垂眼看下（整眼下移留白）+ 左眼外角一滴泪
    img = Image.new('RGBA', size, (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for i, (x0, y0, x1, y1) in enumerate(eyes):
        dx = -2 if i == 0 else 2
        crop = base_eyes.crop((x0, y0, x1 + 1, y1 + 1))
        img.paste(crop, (x0 + dx, y0 + 8), crop)
    tx0, ty0, tx1, ty1 = eyes[0]
    tw = max(6, (tx1 - tx0) // 5)
    tx, ty = tx0 - 1, ty1 + 3
    d.ellipse((tx, ty, tx + tw, ty + int(tw * 1.35)), fill=TEAR_COLOR)
    d.polygon([(tx, ty + 2), (tx + tw // 2, ty - 5), (tx + tw, ty + 2)], fill=TEAR_COLOR)
    out['eyes-sad'] = img
    return out


def gen_mouth(size, box):
    w, h = size
    out = {}
    # 微笑：︶
    img = Image.new('RGBA', size, (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    stroke(d, arc_points(box, up=False, inset=4), width=7, color=MOUTH_COLOR)
    out['mouth-smile'] = img
    # 撇嘴：︵
    img = Image.new('RGBA', size, (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pts = arc_points((box[0] + 2, box[1] + 4, box[2] - 2, box[3] - 8), up=True, inset=2)
    stroke(d, pts, width=6, color=MOUTH_COLOR)
    out['mouth-sad'] = img
    return out


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
        '~/made/plugins/codex/sprite')
    base_eyes_open = Image.open(os.path.join(target, 'eyes-open.png')).convert('RGBA')
    eyes_closed = Image.open(os.path.join(target, 'eyes-closed.png'))
    mouth_open = Image.open(os.path.join(target, 'mouth-open.png')).convert('RGBA')
    mouth_closed = Image.open(os.path.join(target, 'mouth-closed.png'))

    eye_boxes = blob_bboxes(base_eyes_open.split()[3])
    if len(eye_boxes) != 2:
        raise SystemExit(f'eyes-open 连通域数={len(eye_boxes)}（期望 2），拒绝盲改')
    # 嘴的作画区域：开/闭两帧内容的并集 bbox
    mo_box = blob_bboxes(mouth_open.split()[3])
    mc_box = blob_bboxes(mouth_closed.split()[3])
    boxes = mo_box + mc_box
    if not boxes:
        raise SystemExit('mouth 帧没有内容')
    mouth_box = (min(b[0] for b in boxes), min(b[1] for b in boxes),
                 max(b[2] for b in boxes), max(b[3] for b in boxes))
    if not mo_box:
        raise SystemExit('mouth-open 没有内容')

    frames = {}
    frames.update(gen_eyes(base_eyes_open, base_eyes_open.size, eye_boxes))
    frames.update(gen_mouth(mouth_open.size, mouth_box))
    for name, img in frames.items():
        path = os.path.join(target, name + '.png')
        img.save(path, optimize=True)
        print(f'wrote {path} {img.size}')

    # —— manifest：升版本 + 扩 frames + emotions 表 ——
    mpath = os.path.join(target, 'manifest.json')
    m = json.load(open(mpath))
    layers = {L['id']: L for L in m['layers']}
    eyes_l, mouth_l = layers['eyes'], layers['mouth']
    eyes_l['frames'] = ['eyes-open.png', 'eyes-closed.png', 'eyes-focus.png',
                        'eyes-wow.png', 'eyes-happy.png', 'eyes-sad.png']
    mouth_l['frames'] = ['mouth-closed.png', 'mouth-open.png',
                         'mouth-smile.png', 'mouth-sad.png']
    m['emotions'] = {
        'idle':    {'eyes': 0},
        'talking': {'mouth': 'cycle'},
        'running': {'eyes': 2},
        'waiting': {'eyes': 3, 'mouth': 1},
        'offline': {'eyes': 1},
        'success': {'eyes': 4, 'mouth': 2},
        'sad':     {'eyes': 5, 'mouth': 3},
    }
    m['version'] = int(m.get('version', 1)) + 1
    json.dump(m, open(mpath, 'w'), ensure_ascii=False, indent=2)
    print(f'manifest version -> {m["version"]}')
    print('eyes-closed 帧尺寸一致:',
          Image.open(os.path.join(target, 'eyes-closed.png')).size == base_eyes_open.size)


if __name__ == '__main__':
    main()
