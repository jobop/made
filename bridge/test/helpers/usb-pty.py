"""Exercise the real POSIX serial adapter without needing a second ESP32."""
import base64
import json
import os
import select
import sys

master, slave = os.openpty()
os.set_blocking(master, False)
os.set_blocking(sys.stdin.fileno(), False)
print(json.dumps({"port": os.ttyname(slave)}), flush=True)
incoming = b""
pending = bytearray()
try:
    while True:
        readable, writable, _ = select.select(
            [sys.stdin.fileno(), master], [master] if pending else [], [], 1
        )
        if sys.stdin.fileno() in readable:
            data = os.read(sys.stdin.fileno(), 65536)
            if not data:
                break
            incoming += data
            while b"\n" in incoming:
                line, incoming = incoming.split(b"\n", 1)
                command = json.loads(line)
                pending.extend(base64.b64decode(command["write"]))
        if master in writable:
            try:
                count = os.write(master, pending)
                del pending[:count]
            except BlockingIOError:
                pass
        if master in readable:
            try:
                data = os.read(master, 65536)
                if data:
                    print(json.dumps({"data": base64.b64encode(data).decode()}), flush=True)
            except BlockingIOError:
                pass
finally:
    os.close(master)
    os.close(slave)
