#!/usr/bin/env node
// 触发码得截屏：向本机桥接器取会话令牌，POST /api/screenshot（桥接器会向板子下发
// debug.screenshot 命令并等待固件回传），把返回的 PNG 存到指定路径。
//
// 用法：
//   npm run shot                    # 存到 state/last-shot.png
//   npm run shot -- /tmp/shot.png   # 指定输出路径
//   VIBE_PORT=9000 npm run shot     # 桥接器面板不在默认 8787 时
import fs from 'node:fs';
import path from 'node:path';

const port = Number(process.env.VIBE_PORT) || 8787;
const base = `http://127.0.0.1:${port}`;
const output = process.argv[2] || path.join(process.cwd(), 'state', 'last-shot.png');

const fail = (message) => {
  console.error(`截屏失败：${message}`);
  process.exit(1);
};

try {
  const session = await fetch(`${base}/api/session`).then((r) => r.json());
  if (!session.token) fail('拿不到面板会话令牌，桥接器是否在运行？');
  const response = await fetch(`${base}/api/screenshot`, {
    method: 'POST',
    headers: { 'x-vibe-token': session.token },
  });
  if (!response.ok) {
    const detail = await response.json().catch(() => ({}));
    fail(detail.error || `HTTP ${response.status}`);
  }
  const png = Buffer.from(await response.arrayBuffer());
  fs.mkdirSync(path.dirname(path.resolve(output)), { recursive: true });
  fs.writeFileSync(output, png);
  console.log(`${output} (${png.length} bytes, ${response.headers.get('x-screenshot-file') || 'board'})`);
} catch (error) {
  if (error.cause?.code === 'ECONNREFUSED') fail(`连不上桥接器 ${base}，先 npm start 启动`);
  fail(error.message);
}
