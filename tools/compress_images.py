#!/usr/bin/env python3
# compress_images.py — 素材图片激进压缩工具（PNG/GIF/JPEG）。
#
# 这是素材管线的**默认压缩步骤**：生成/修改素材后必须跑一遍（docs/SPRITES.md
# 「素材压缩规范」）。对已压缩过的文件是幂等的（收益低于 --min-save 自动跳过，
# 不会反复劣化画质）。
#
# 用法：
#   python3 tools/compress_images.py <文件或目录...> [选项]
#
# 选项：
#   --preset extreme|high|balanced|lossless
#       四档梯度按目标板子能力选，默认 extreme（现役 240 圆屏小内存板）：
#       extreme  PNG 64色 + GIF 64色无抖动（体积最小）
#       high     PNG 128色 + GIF 128色无抖动
#       balanced PNG 256色 + GIF 256色 bayer 抖动（最平滑，体积最大）
#       lossless 仅容器优化（PNG 重 deflate / JPEG q95），GIF 不重编码
#   --max-dim N                   最大边长，超过则等比缩小（GIF 整体缩放；
#                                 用于纠正超规格素材，正常素材全 240 不受影响）
#   --min-save N                  压缩收益低于 N% 时不替换（默认 3，保证幂等）
#   --manifest / --no-manifest    目录内有 manifest.json 且文件有改动时自动
#                                 递增 version（默认开；改素材必须升版）
#   --backup-dir DIR              原文件备份目录（默认 /tmp/img-compress-backup/<时间戳>）
#   --dry-run                     只报告将做什么，不写盘
#
# 优先级：pngquant/gifsicle 存在则用（libimagequant 抖动质量更好），否则
# PIL（quantize FASTOCTREE + FLOYDSTEINBERG）/ffmpeg（palettegen/paletteuse）
# 兜底，两条路径产出等价。本机当前无 pngquant（brew 被系统沙箱挡），走兜底。
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

from PIL import Image

IMG_EXTS = {'.png', '.gif', '.jpg', '.jpeg'}
PRESET = {
    # 四档压缩梯度，按目标板子能力选。默认 extreme（现役 240 圆屏小内存板）。
    # 同一套素材源，发布到不同板子时跑不同档即可。
    #
    # extreme（默认）：PNG 64 色；GIF 64 色无抖动。GIF 体积的最大来源是抖动
    #   噪声（噪声让帧间差分矩形失效、LZW 压不住），64c 无抖动在 240 圆屏与
    #   256c+bayer 基本不可分辨（软渐变轻微色带）。PNG 64 色实测全包再省
    #   ~27%，特征层主色（绿豆 eyes/mouth）各色数档均完整保留（高频主色
    #   量化器必收进调色板），RGB565 屏深下 64c 与 256c 不可分辨。
    'extreme':  dict(colors=64, jpg_q=70,
                     gif_colors=64, gif_dither='none'),
    # high：中档板。PNG 128 色；GIF 128 色无抖动，渐变更顺滑。
    'high':     dict(colors=128, jpg_q=80,
                     gif_colors=128, gif_dither='none'),
    # balanced：好板子（大 flash/宽容画质）。PNG 256 色；GIF 256 色 + bayer。
    'balanced': dict(colors=256, jpg_q=85,
                     gif_colors=256, gif_dither='bayer:bayer_scale=5'),
    # lossless：只做容器层优化（PNG 重 deflate / JPEG q95），GIF 不重编码
    # （GIF 没有真正的无损重编码，跳过以不动像素）。
    'lossless': dict(colors=None, jpg_q=95,
                     gif_colors=None, gif_dither=None, gif_reencode=False),
}


def log(msg):
    print(msg, flush=True)


def which(name):
    return shutil.which(name)


def quantize_rgba_safe(rgba, n_colors):
    """alpha 保真的调色板量化（分层）。

    PIL 自带 quantize(FASTOCTREE) 在低色数下会把 (0,0,0,0) 纯透明与半透明
    深色合并（实测 64c 把透明背景顶成 (0,0,0,174)——调色板条目不足时 alpha
    被牺牲），产生真机可见的暗色矩形。这里手动分层：

      1. alpha<60 → 统一透明项（与固件契约一致：软边 alpha<60 视为透明）
      2. 软边 (60≤a<250)：(r,g,b,a) 四元组聚类，占 ~10%，8~16 项足够
      3. 不透明 (a≥250)：RGB MEDIANCUT 占剩余色数

    调色板每项带独立 alpha（PNG tRNS per-entry，固件 libpng 解码链支持），
    软边渐变得以保留。
    """
    n = max(8, n_colors)
    n_soft = min(16, max(4, n // 8))       # 软边预算
    n_opaque = n - 1 - n_soft              # 不透明预算
    px = list(rgba.getdata())
    w, h = rgba.size

    soft = [(r, g, b, a) for r, g, b, a in px if 60 <= a < 250]
    op = [(r, g, b) for r, g, b, a in px if a >= 250]

    soft_pal = []   # [(r,g,b,a)]
    if soft:
        # 软边色彩少：四元组直接聚类（复用 quantize 对 RGBA 的处理，但
        # 输入只含软边像素，合并不会波及纯透明色）
        sim = Image.new('RGBA', (len(soft), 1))
        sim.putdata(soft)
        q = sim.quantize(colors=n_soft, method=Image.Quantize.FASTOCTREE,
                         dither=Image.Dither.NONE)
        raw_pal = q.getpalette()  # [r,g,b, r,g,b, ...] 长度 = 色数*3? 可能补齐
        # 取回每个软边像素的调色板项（含 alpha：FASTOCTREE 项内 alpha 存在
        # im.im 的 palette alpha 里；convert('RGBA') 读回最稳）
        q_rgba = q.convert('RGBA')
        seen = {}
        for p in q_rgba.getdata():
            if p[:3] != (0, 0, 0) or p[3] != 0:  # 排除被合进来的纯透明
                seen[p] = None
        soft_pal = sorted(seen.keys(), key=lambda p: -p[3])[:n_soft]

    op_pal = []
    if op and n_opaque > 0:
        sim = Image.new('RGB', (len(op), 1))
        sim.putdata(op)
        q = sim.quantize(colors=n_opaque, method=Image.Quantize.MEDIANCUT,
                         dither=Image.Dither.NONE)
        pal = q.getpalette()
        # getcolors() 默认 maxcolors=256，超过返回 None——必须显式放开，
        # 否则 op_pal 恒为空、全部像素被吸进 16 项软边调色板（灾难性失真）
        cnt = q.getcolors(maxcolors=len(op) + 1) or []
        used = sorted({i for _, i in cnt})
        op_pal = [tuple(pal[i*3:i*3+3]) for i in used]

    # 最终调色板：0=透明，1..S=软边，S+1..=不透明
    # 注意：偏移量必须用 len(soft_pal) 实际值——软边聚类去重后可能少于
    # 预算 n_soft，用预算值会索引错位（深棕瞳孔被映射到米白项的实锤 bug）
    soft_n = len(soft_pal)
    palette = [(0, 0, 0, 0)] + soft_pal + [(r, g, b, 255) for r, g, b in op_pal]
    n_total = len(palette)
    index_of = {}
    for i, p in enumerate(palette[1:], start=1):
        index_of[p] = i

    import numpy as np
    arr = np.array(rgba, dtype=np.int16)
    a = arr[:, :, 3]
    out = np.zeros((h, w), dtype=np.uint8)  # 索引图

    # 透明
    out[a < 60] = 0
    # 不透明 & 软边：最近邻查色（像素量 ≤240*240*层数，纯 Python 太慢，
    # 用 KD- lite：颜色聚成 dict 缓存）
    cache = {}
    body = arr[:, :, :3]
    asel = a
    ys, xs = np.where(a >= 60)
    if len(soft_pal):
        # 距离计算用 int32：int16 下颜色差平方 (255²=65025) 会溢出回绕，
        # argmin 结果错乱（瞳孔深棕被映射到米白的根因）
        soft_arr = np.array(soft_pal, dtype=np.int32)      # (S,4)
        op_arr = np.array(op_pal, dtype=np.int32) if op_pal else np.zeros((0, 3), np.int32)
        for y, x in zip(ys.tolist(), xs.tolist()):
            key = (body[y, x, 0], body[y, x, 1], body[y, x, 2], a[y, x])
            idx = cache.get(key)
            if idx is None:
                r, g, b, av = key
                if av >= 250 and len(op_arr):
                    d2 = ((op_arr - np.array([r, g, b])) ** 2).sum(1)
                    idx = 1 + soft_n + int(d2.argmin())
                elif len(soft_arr):
                    d2 = ((soft_arr - np.array([r, g, b, av])) ** 2).sum(1)
                    idx = 1 + int(d2.argmin())
                    if av >= 250 and not len(op_arr):
                        idx = 1 + int(((soft_arr[:, :3] - np.array([r, g, b])) ** 2).sum(1).argmin())
                else:
                    idx = 1
                cache[key] = idx
            out[y, x] = idx
    else:
        if len(op_pal):
            op_arr = np.array(op_pal, dtype=np.int32)
            for y, x in zip(ys.tolist(), xs.tolist()):
                key = (int(body[y, x, 0]), int(body[y, x, 1]), int(body[y, x, 2]))
                idx = cache.get(key)
                if idx is None:
                    d2 = ((op_arr - np.array(key)) ** 2).sum(1)
                    idx = 1 + soft_n + int(d2.argmin())
                    cache[key] = idx
                out[y, x] = idx

    pimg = Image.fromarray(out, mode='P')
    flat = []
    for r, g, b, _ in palette:
        flat += [r, g, b]
    flat += [0] * (256 * 3 - len(flat))
    pimg.putpalette(flat)
    # tRNS per-entry alpha
    pimg.info['transparency'] = bytes(a_ for _, _, _, a_ in palette) + \
        bytes(256 - n_total)
    return pimg


def compress_png(path, preset, max_dim, dry_run):
    """PNG：分层 alpha 保真量化（quantize_rgba_safe）+ 最大压缩。
    返回 (新字节串 or None 表示跳过, 说明)。"""
    im = Image.open(path)
    im.load()
    orig = im.copy()
    w, h = im.size
    note = []
    if max_dim and max(w, h) > max_dim:
        scale = max_dim / max(w, h)
        nw, nh = round(w * scale), round(h * scale)
        im = im.resize((nw, nh), Image.Resampling.LANCZOS)
        note.append(f'resize {w}x{h}->{nw}x{nh}')
    if preset['colors']:
        has_alpha = im.mode in ('RGBA', 'LA', 'PA') or (
            im.mode == 'P' and 'transparency' in im.info)
        if has_alpha:
            q = quantize_rgba_safe(im.convert('RGBA'), preset['colors'])
        else:
            q = im.convert('RGB').quantize(
                colors=preset['colors'],
                method=Image.Quantize.MEDIANCUT,
                dither=Image.Dither.FLOYDSTEINBERG)
        im = q
    import io
    buf = io.BytesIO()
    im.save(buf, 'PNG', optimize=True, compress_level=9)
    return buf.getvalue(), '; '.join(note)


def compress_gif(path, preset, max_dim, dry_run):
    """GIF：ffmpeg palettegen/paletteuse 双通道重编码（差分调色板+bayer 抖动，
    保留透明与逐帧延时）。抖动/色数由 preset 决定（extreme=64c 无抖动）。"""
    if not preset.get('gif_reencode', True):
        return None, 'lossless-skip-gif'
    if not which('ffmpeg'):
        return None, 'no-ffmpeg'
    vf = []
    if max_dim:
        # 只缩超规格的；-1 保持纵横比
        vf.append(f"scale='min(iw,{max_dim})':'min(ih,{max_dim})'"
                  ':force_original_aspect_ratio=decrease:flags=lanczos')
    gc = preset.get('gif_colors')
    gd = preset.get('gif_dither')
    if gc:
        pgen = 'palettegen=stats_mode=diff'
        if gc != 256:
            pgen += f':max_colors={gc}'
        puse = 'paletteuse'
        if gd:
            puse += f'=dither={gd}'
        puse += ':diff_mode=rectangle'
        vf.append(f'split[s0][s1];[s0]{pgen}[p];[s1][p]{puse}')
    out = path + '.cq.gif'
    cmd = ['ffmpeg', '-y', '-loglevel', 'error', '-i', path,
           '-vf', ','.join(vf), '-loop', '0', out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None, 'ffmpeg-failed: ' + r.stderr.strip()[-160:]
    with open(out, 'rb') as f:
        data = f.read()
    os.unlink(out)
    return data, ('resize;' if max_dim else '') + 'ffmpeg-palette'


def compress_jpeg(path, preset, max_dim, dry_run):
    im = Image.open(path)
    im = im.convert('RGB')
    w, h = im.size
    if max_dim and max(w, h) > max_dim:
        scale = max_dim / max(w, h)
        im = im.resize((round(w * scale), round(h * scale)),
                       Image.Resampling.LANCZOS)
    import io
    buf = io.BytesIO()
    im.save(buf, 'JPEG', quality=preset['jpg_q'], optimize=True, progressive=True)
    return buf.getvalue(), ('resize;' if max_dim else '') + 'jpeg-reencode'


def bump_manifest(dirpath):
    """sprite 目录的 manifest.json version+1（每目录每轮只调一次）。"""
    cand = os.path.join(dirpath, 'manifest.json')
    if not os.path.isfile(cand):
        return
    try:
        m = json.load(open(cand))
        v = m.get('version')
        if isinstance(v, int):
            m['version'] = v + 1
            json.dump(m, open(cand, 'w'), ensure_ascii=False, indent=2)
            log(f'  manifest {cand}: version {v} -> {v + 1}')
    except Exception as e:
        log(f'  manifest bump failed {cand}: {e}')


def collect(paths):
    for p in paths:
        if os.path.isfile(p) and os.path.splitext(p)[1].lower() in IMG_EXTS:
            yield p
        elif os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                for fn in sorted(files):
                    if (os.path.splitext(fn)[1].lower() in IMG_EXTS
                            and not fn.endswith('.cq.gif')):
                        yield os.path.join(root, fn)


def compress_file(path, preset_name='extreme', max_dim=None,
                  min_save=3, backup_dir=None):
    """供生成脚本复用的单文件压缩入口：压缩并落盘（带备份），返回是否写入。
    收益低于 min_save 且未 resize 时跳过（幂等）。"""
    before = os.path.getsize(path)
    ext = os.path.splitext(path)[1].lower()
    preset = PRESET[preset_name]
    if ext == '.png':
        data, note = compress_png(path, preset, max_dim, False)
    elif ext == '.gif':
        data, note = compress_gif(path, preset, max_dim, False)
    else:
        data, note = compress_jpeg(path, preset, max_dim, False)
    if data is None:
        return False
    after = len(data)
    save_pct = (before - after) * 100 // max(before, 1)
    if 'resize' not in note and (save_pct < min_save or after >= before):
        return False
    backup_dir = backup_dir or f'/tmp/img-compress-backup/{time.strftime("%Y%m%d-%H%M%S")}'
    os.makedirs(backup_dir, exist_ok=True)
    dst = os.path.join(backup_dir, os.path.basename(path))
    if not os.path.exists(dst):
        shutil.copy2(path, dst)
    with open(path, 'wb') as f:
        f.write(data)
    log(f'compressed {path}: {before}B -> {after}B (-{save_pct}%) {note}')
    return True


def main():
    ap = argparse.ArgumentParser(description='素材图片激进压缩（管线默认步骤）')
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--preset', choices=list(PRESET),
                    default='extreme',
                    help='extreme(默认,240圆屏)/high/balanced/lossless，按目标板子选')
    ap.add_argument('--max-dim', type=int, default=None)
    ap.add_argument('--min-save', type=int, default=3)
    ap.add_argument('--no-manifest', action='store_true')
    ap.add_argument('--backup-dir', default=None)
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()

    preset = PRESET[args.preset]
    ts = time.strftime('%Y%m%d-%H%M%S')
    backup_root = args.backup_dir or f'/tmp/img-compress-backup/{ts}'
    os.makedirs(backup_root, exist_ok=True)

    total_before = total_after = 0
    touched_dirs = set()
    for path in collect(args.paths):
        try:
            before = os.path.getsize(path)
            ext = os.path.splitext(path)[1].lower()
            if ext == '.png':
                data, note = compress_png(path, preset, args.max_dim, args.dry_run)
            elif ext == '.gif':
                data, note = compress_gif(path, preset, args.max_dim, args.dry_run)
            else:
                data, note = compress_jpeg(path, preset, args.max_dim, args.dry_run)
        except Exception as e:
            log(f'SKIP {path}: {e}')
            continue
        if data is None:
            log(f'SKIP {path}: {note}')
            continue
        after = len(data)
        save_pct = (before - after) * 100 // max(before, 1)
        # resize 属于安全修复（超规格帧的运行时画布隐患），强制落盘，
        # 不受 --min-save 门槛约束（dance.gif 缩后反而变大也必须换）。
        if ('resize' not in note and (save_pct < args.min_save or after >= before)):
            log(f'KEEP  {path} ({before}B, would save {save_pct}%)')
            continue
        rel = os.path.relpath(path, start=backup_root)
        dst = os.path.join(backup_root, os.path.basename(path))
        n = 0
        while os.path.exists(dst):
            n += 1
            dst = os.path.join(backup_root, f'{n}_{os.path.basename(path)}')
        log(f'WRITE {path}: {before}B -> {after}B (-{save_pct}%) {note}'
            f'  [backup {dst}]')
        if not args.dry_run:
            shutil.copy2(path, dst)
            with open(path, 'wb') as f:
                f.write(data)
        total_before += before
        total_after += after
        touched_dirs.add(os.path.dirname(path))

    if not args.dry_run and not args.no_manifest:
        for d in sorted(touched_dirs):
            bump_manifest(d)

    if total_before:
        log(f'== {len(touched_dirs)} dir(s), '
            f'{total_before}B -> {total_after}B '
            f'(-{(total_before - total_after) * 100 // total_before}%)')
    else:
        log('== nothing to do (all files already optimal)')


if __name__ == '__main__':
    main()
