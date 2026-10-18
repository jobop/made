#!/usr/bin/env python3
# bean-fall.py — 给绿豆素材包补「跌倒」(fall) 动作帧（行动系统素材侧）。
#
# 用法：python3 bean-fall.py <sprite目录>
# （默认 ~/made/plugins/codex/sprite）
#
# 派生（幂等，可重复执行，直接覆盖同名输出）：
#   body-fall-a.png / body-fall-b.png — body.png 整体左/右倾斜 ±12°（同画布
#   360，避免层坐标漂移）；eyes-x.png — 从 eyes-open 的逐眼连通域几何画 X
#   晕眼（帧尺寸与既有眼帧一致，同层共用 x/y）。
# manifest：version 递增、body/eyes frames 扩容、写 actions.fall 时序表。
#
# 约束（docs/SPRITES.md）：eyes 层帧数上限 8；动作引用的层不参与呼吸烘焙
# （固件已排除）；动作=对既有层帧序的关键帧覆盖，没有新解码路径。
import json
import math
import os
import sys
from collections import deque

from PIL import Image, ImageDraw

TILT_DEG = 12.0
EYE_FALLBACK_COLOR = (3, 33, 12, 255)
ALPHA_TH = 60


def blob_bboxes(alpha, thresh=ALPHA_TH):
    """alpha 阈值化后的连通域 bbox（4 邻接，按 x 排序 = 左眼在前）。"""
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


def dominant_color(im, alpha):
    """连通域里出现次数最多的不透明颜色（含轻微抗锯齿的容差）。"""
    px, ap = im.load(), alpha.load()
    w, h = im.size
    counts = {}
    for y in range(h):
        for x in range(w):
            if ap[x, y] < 200:
                continue
            counts[px[x, y]] = counts.get(px[x, y], 0) + 1
    return max(counts, key=counts.get) if counts else EYE_FALLBACK_COLOR


def make_tilt(body_path, out_path, deg):
    """body 整体旋转 deg 度，画布尺寸保持不变（层 x/y 不漂移）。"""
    im = Image.open(body_path).convert("RGBA")
    # rotate 逆时针为正；resample bicubic + transparent 背景不破坏软边 alpha
    tilted = im.rotate(deg, resample=Image.BICUBIC, expand=False,
                       fillcolor=(0, 0, 0, 0))
    tilted.save(out_path)
    return tilted.size


def make_x_eyes(eyes_open_path, out_path):
    """从 eyes-open 的逐眼 bbox 画 X 晕眼（帧尺寸与眼帧一致）。"""
    im = Image.open(eyes_open_path).convert("RGBA")
    alpha = im.split()[-1]
    boxes = blob_bboxes(alpha)
    if not boxes:
        raise SystemExit("eyes-open 里没找到眼瞳连通域")
    color = dominant_color(im, alpha)
    out = Image.new("RGBA", im.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(out)

    def stroke(pts, width):
        draw.line(pts, fill=color, width=width, joint="curve")
        r = (width - 1) // 2 + 1
        for (x, y) in (pts[0], pts[-1]):
            draw.ellipse((x - r, y - r, x + r, y + r), fill=color)

    for (x0, y0, x1, y1) in boxes:
        w, h = x1 - x0, y1 - y0
        pad = min(w, h) * 0.18
        cx0, cy0, cx1, cy1 = x0 + pad, y0 + pad, x1 - pad, y1 - pad
        width = max(3, int(min(w, h) * 0.22))
        stroke([(cx0, cy0), (cx1, cy1)], width)
        stroke([(cx1, cy0), (cx0, cy1)], width)
    out.save(out_path)
    return len(boxes)


def main():
    sprite_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
        "~/made/plugins/codex/sprite")
    manifest_path = os.path.join(sprite_dir, "manifest.json")
    with open(manifest_path, encoding="utf-8") as f:
        manifest = json.load(f)

    layers = {layer["id"]: layer for layer in manifest["layers"]}

    # 1) body 倾斜帧（画布同尺寸）。单帧 file 形态先转 frames 数组（固件解析
    #    frames 优先、file 兜底，转完兼容两者）。
    body = layers["body"]
    if "frames" not in body:
        body["frames"] = [body["file"]] if body.get("file") else []
    for suffix, deg in (("a", TILT_DEG), ("b", -TILT_DEG)):
        name = f"body-fall-{suffix}.png"
        size = make_tilt(os.path.join(sprite_dir, "body.png"),
                         os.path.join(sprite_dir, name), deg)
        if name not in body["frames"]:
            body["frames"].append(name)
        print(f"body frame {name} {size} deg={deg}")

    # 2) X 晕眼
    eyes = layers["eyes"]
    count = make_x_eyes(os.path.join(sprite_dir, "eyes-open.png"),
                        os.path.join(sprite_dir, "eyes-x.png"))
    if "eyes-x.png" not in eyes["frames"]:
        eyes["frames"].append("eyes-x.png")
    print(f"eyes frame eyes-x.png blobs={count}")

    if len(eyes["frames"]) > 8:
        raise SystemExit("eyes 层帧数超过 MAX_FRAMES=8")

    # 3) actions.fall 时序：后仰 → 歪倒 → 回正，晕眼收尾睁眼
    body_n = len(body["frames"])
    eyes_n = len(eyes["frames"])
    manifest["actions"] = manifest.get("actions") or {}
    manifest["actions"]["fall"] = {
        "frame_ms": 140,
        "loops": 1,
        "tracks": {
            "body": [body_n - 2, body_n - 1, body_n - 2, body_n - 1, 0, 0],
            "eyes": [eyes_n - 1] * 4 + [0, 0],
        },
    }

    manifest["version"] = int(manifest.get("version", 1)) + 1
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"manifest version -> {manifest['version']}, actions.fall written")


if __name__ == "__main__":
    main()
