#!/usr/bin/env python3
"""nano banana (Gemini 2.5 Flash Image) 参考编辑 bake-off 测试。

用法：
  GEMINI_API_KEY=xxx python3 gemini-bakeoff.py <基图.png> <输出目录>

对每个情绪提示词调用一次参考编辑，输出原图和自动质检结果：
  - 底色纯度（四角+边缘白度）
  - 与基图的身体差异（五官区域外的变化面积）
  - 五官 bbox 提取（落到 <out>/<情绪>.png 供管线消费）
"""
import base64
import json
import os
import sys
import urllib.request

API = ("https://generativelanguage.googleapis.com/v1beta/models/"
       "gemini-2.5-flash-image:generateContent")

# 与管线验收一致的硬性提示词骨架：锁构图/平涂/纯底
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


def call_gemini(key: str, base_png: bytes, edit: str) -> bytes:
    prompt = (
        "Edit this cartoon cat character: keep the body, pose, colors, collar "
        "and bell EXACTLY unchanged. Only change the face: " + edit + ". "
        "Flat vector cartoon style, no gradients, no shading, pure white "
        "background, no other changes anywhere."
    )
    body = json.dumps({
        "contents": [{
            "parts": [
                {"inline_data": {"mime_type": "image/png", "data": base64.b64encode(base_png).decode()}},
                {"text": prompt},
            ]
        }],
        "generationConfig": {"responseModalities": ["IMAGE"]},
    }).encode()
    req = urllib.request.Request(
        API, data=body, method="POST",
        headers={"Content-Type": "application/json", "x-goog-api-key": key})
    with urllib.request.urlopen(req, timeout=120) as r:
        resp = json.load(r)
    for part in resp["candidates"][0]["content"]["parts"]:
        if "inlineData" in part:
            return base64.b64decode(part["inlineData"]["data"])
    raise SystemExit("响应里没有图片: " + json.dumps(resp)[:500])


def main():
    key = os.environ.get("GEMINI_API_KEY")
    if not key:
        raise SystemExit("需要 GEMINI_API_KEY 环境变量")
    base_path, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    base_png = open(base_path, "rb").read()
    for name, edit in PROMPTS.items():
        out = os.path.join(out_dir, f"{name}.png")
        open(out, "wb").write(call_gemini(key, base_png, edit))
        print(f"[ok] {out}")
    print("质检与五官提取交给 check-emotions.py（与内置管线共用）")


if __name__ == "__main__":
    main()
