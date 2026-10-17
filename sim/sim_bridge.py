#!/usr/bin/env python3
"""made_sim_bridge.py — sim 模拟器专用桩桥接器（:8788）。

复刻真机「老鼠助手插件」场景：一条已完成任务 + 超长 markdown 回复，
用于在 sim 里复现/验证任务页布局（徽章溢出、文字出卡、对齐问题）。
"""
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

SESSION_ID = "a1b2c3d4-1111-4222-8333-444455556666"

RESULT_TEXT = (
    "- **形象素材** `sprite/`：圆滚滚奶杏色小仓鼠，樱粉内耳 + 粉腮红 + 紫"
    "果红小围巾，怀里还捧着颗葵花籽；expressive 档——4 秒眨一次眼、说话"
    "张嘴；各层像素尺寸都已过自检，不用再特意检查手。\n"
    "- **动效注册** `plugin.json` 的 sprite 段已指向新素材包，blink/talk "
    "帧率与旧版一致；idle 呼吸为 12 帧循环，首尾帧相同保证循环无缝。\n"
    "- **市场元数据**：market.json 版本号 1.0.4→1.0.5，changelog 已补"
    "「仓鼠新形象」条目；README 的预览图换成三帧连招。\n"
    "- **自检**：validate 通过，桌面端预览确认五层对位无偏移；上线后记得"
    "让设备重新拉取素材缓存。"
)

CONFIG = {
    "providers": [
        {
            "id": "pi_agent",
            "label": "老鼠助手插件",
            "available": True,
            "capabilities": {"cancel": True, "progress": True},
        },
        {
            "id": "codex",
            "label": "绿豆助手",
            "available": True,
            "capabilities": {"cancel": True},
        },
    ],
    "voice": {"available": True},
    "locale": "zh-CN",
}

SESSIONS = {
    "pi_agent": [
        {"id": SESSION_ID, "title": "老鼠助手插件", "provider": "pi_agent", "projectId": "proj1"},
    ],
    "codex": [],
}

TASKS = {
    SESSION_ID: [
        {
            "id": "job-1",
            "provider": "pi_agent",
            "projectId": "proj1",
            "vibeSessionId": SESSION_ID,
            "instruction": "帮我做一个「老鼠」助手插件，形象是圆滚滚的小仓鼠",
            "status": "completed",
            "result": RESULT_TEXT,
        }
    ]
}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, payload: dict, status: int = 200):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        query = self.path.split("?")[1] if "?" in self.path else ""
        params = dict(kv.split("=", 1) for kv in query.split("&") if "=" in kv)
        if path == "/device/config":
            return self._send(CONFIG)
        if path == "/device/sessions":
            return self._send({"sessions": SESSIONS.get(params.get("provider", ""), [])})
        if path == "/device/tasks":
            return self._send({"jobs": TASKS.get(params.get("sessionId", ""), [])})
        if path == "/device/commands":
            return self._send({"commands": []})
        if path.startswith("/device/heartbeat"):
            return self._send({"ok": True})
        return self._send({"error": "not found"}, 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length:
            self.rfile.read(length)
        return self._send({"ok": True})

    def log_message(self, fmt, *args):
        print("[sim-bridge]", self.address_string(), fmt % args)


if __name__ == "__main__":
    print("sim bridge listening on :8788")
    HTTPServer(("127.0.0.1", 8788), Handler).serve_forever()
