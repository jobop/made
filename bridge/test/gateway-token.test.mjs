// 云端网关令牌测试：VIBE_API_GATEWAY_TOKEN 设置后，设备口/桌面口的所有请求都必须带
// x-made-cloud-gateway 头（e2b 等公网可达部署的安全边界）。未设置时行为不变。
// 运行：node --test test/gateway-token.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.join(here, '..');

async function testGatewayToken(enabled) {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-gw-'));
  const port = 28000 + Math.floor(Math.random() * 2000) * 3;
  const cfg = {
    port: port + 1, // 桌面口
    devicePort: port, // 设备口
    discoveryPort: port + 2,
    defaultProject: 'demo',
    workspaceRoot: './workspaces',
    projects: [{ id: 'demo', label: 'demo', path: tmp }],
    pluginSettings: {},
  };
  const cfgPath = path.join(tmp, 'config.json');
  fs.writeFileSync(cfgPath, JSON.stringify(cfg, null, 2));
  const env = {
    ...process.env,
    VIBE_CONFIG: cfgPath,
    VIBE_STATE_DIR: path.join(tmp, 'state'),
    VIBE_DATA_DIR: path.join(tmp, 'data'),
    VIBE_USB_DISABLE: '1',
    VIBE_DISCOVERY_PORT: String(port + 2),
    ...(enabled ? { VIBE_API_GATEWAY_TOKEN: 'sekret' } : {}),
  };
  const child = spawn(process.execPath, [path.join(root, 'src', 'server.mjs')], {
    env, stdio: ['ignore', 'pipe', 'pipe'],
  });
  child.stderr.on('data', () => {});
  const device = `http://127.0.0.1:${port}`;
  const desktop = `http://127.0.0.1:${port + 1}`;
  try {
    // 等服务就绪
    const deadline = Date.now() + 20000;
    let up = false;
    while (Date.now() < deadline) {
      try {
        const res = await fetch(`${desktop}/api/session`, {
          headers: enabled ? { 'x-made-cloud-gateway': 'sekret' } : {},
          signal: AbortSignal.timeout(1000),
        });
        if (res.status === 200) { up = true; break; }
      } catch { /* 未就绪 */ }
      await new Promise((r) => setTimeout(r, 300));
    }
    assert.ok(up, '桥接器未在超时内就绪');

    if (!enabled) {
      // 未设令牌：行为与从前完全一致（本机访问直通）
      const res = await fetch(`${desktop}/api/session`);
      assert.equal(res.status, 200);
      return;
    }

    // 桌面口：无令牌 403，有令牌 200
    const noTok = await fetch(`${desktop}/api/session`);
    assert.equal(noTok.status, 403);
    const wrongTok = await fetch(`${desktop}/api/session`, { headers: { 'x-made-cloud-gateway': 'wrong' } });
    assert.equal(wrongTok.status, 403);
    const withTok = await fetch(`${desktop}/api/session`, { headers: { 'x-made-cloud-gateway': 'sekret' } });
    assert.equal(withTok.status, 200);
    const body = await withTok.json();
    assert.ok(body.token, '应返回本机操作令牌');

    // 设备口：无令牌 403，有令牌放行（后续校验按路由自身逻辑走）
    const dNoTok = await fetch(`${device}/pair/status?deviceId=abc&nonce=abc`);
    assert.equal(dNoTok.status, 403);
    const dWithTok = await fetch(`${device}/pair/status?deviceId=abc&nonce=abc`, {
      headers: { 'x-made-cloud-gateway': 'sekret' },
    });
    assert.notEqual(dWithTok.status, 403);
  } finally {
    child.kill('SIGTERM');
    fs.rmSync(tmp, { recursive: true, force: true });
  }
}

test('未设置网关令牌：桌面口本机访问行为不变', async () => { await testGatewayToken(false); });

test('设置网关令牌：两口都要求 x-made-cloud-gateway', async () => { await testGatewayToken(true); });
