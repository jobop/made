#!/usr/bin/env python3
"""混元 Hy-Image-3.5-preview（TokenHub）参考编辑 bake-off。

用法：
  HY_API_KEY=xxx python3 hy-bakeoff.py <基图.png> <输出目录>

与 gemini-bakeoff.py 同一套情绪提示词，输出可并排对比。
接口：TokenHub Chat/Messages 协议（参考腾讯云文档 product/1823）。
"""
import base64
import json
import os
import sys
import urllib.request

# 国内站 TokenHub 端点；若用国际站改为 intl tokenhub-intl.tencentcloudmaas.com
API = "https://tokenhub.tencentcloudmaas.com/v1/wand/hunyuan-image"

PROMPTS = {
    "happy": ("replace the blank face with happy squinted eyes drawn as two thick "
              "upward arcs and a small open smiling mouth below"),
    "wow": ("replace the blank face with two small round surprised eyes and a "
            "small round open mouth (shocked expression)"),
    "focus": ("replace the blank face with half-lidded focused eyes (straight "
              "upper eyelid, looking down slightly) and a small flat closed mouth"),
    "sad": ("replace the blank face with drooping eyes looking down and a small "
            "downturned pouting mouth, one tiny teardrop at the outer corner of "
            "the left eye"),
}


def call_hy(key: str, base_png: bytes, edit: str) -> bytes:
    """messages 协议单轮编辑：text + 参考图，取回图。"""
    prompt = (
        "Edit this cartoon cat character: keep the body, pose, colors, collar "
        "and bell EXACTLY unchanged. Only change the face: " + edit + ". "
        "Flat vector cartoon style, no gradients, no shading, pure white "
        "background, no other changes anywhere."
    )
    data_uri = ("data:image/png;base64,"
                + base64.b64encode(base_png).decode())
    body = json.dumps({
        "model": "hy-image-v3.5-preview",
        "messages": [{
            "role": "user",
            "content": [
                {"type": "image_url", "image_url": {"url": data_uri}},
                {"type": "text", "text": prompt},
            ],
        }],
    }).encode()
    req = urllib.request.Request(
        API + "/chat/completions", data=body, method="POST",
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer " + key})
    with urllib.request.urlopen(req, timeout=300) as r:
        resp = json.load(r)
    # 从响应中提取 base64 图片（兼容 choices[0].message.images / content 内嵌）
    msg = resp["choices"][0]["message"]
    imgs = msg.get("images") or []
    for item in imgs:
        url = item.get("image_url", {}).get("url", "")
        if url.startswith("data:"):
            return base64.b64decode(url.split(",", 1)[1])
    content = msg.get("content")
    if isinstance(content, list):
        for part in content:
            if part.get("type") == "image_url":
                url = part["image_url"]["url"]
                if url.startswith("data:"):
                    return base64.b64decode(url.split(",", 1)[1])
    raise SystemExit("响应里没有图片: " + json.dumps(resp)[:800])


def main():
    key = os.environ.get("HY_API_KEY")
    if not key:
        raise SystemExit("需要 HY_API_KEY 环境变量")
    base_path, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    base_png = open(base_path, "rb").read()
    for name, edit in PROMPTS.items():
        out = os.path.join(out_dir, f"{name}.png")
        open(out, "wb").write(call_hy(key, base_png, edit))
        print(f"[ok] {out}")


if __name__ == "__main__":
    main()
