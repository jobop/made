import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import dgram from 'node:dgram';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { rgb565ToPng, validateScreenshotParams, createScreenshotStore } from '../src/board-screenshot.mjs';

// —— PNG 解析（仅测试用）：拆 chunk，返回 IHDR 与解压后的像素原始流 ——
function decodePng(png) {
  assert.deepEqual([...png.subarray(0, 8)], [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  let offset = 8;
  let ihdr = null;
  const idat = [];
  while (offset < png.length) {
    const length = png.readUInt32BE(offset);
    const type = png.toString('ascii', offset + 4, offset + 8);
    const data = png.subarray(offset + 8, offset + 8 + length);
    if (type === 'IHDR') ihdr = data;
    if (type === 'IDAT') idat.push(data);
    offset += 12 + length;
  }
  return { ihdr, raw: zlib.inflateSync(Buffer.concat(idat)) };
}

test('rgb565ToPng 颜色转换正确，RGB565 三原色映射到 RGB888', () => {
  const width = 16;
  const height = 16;
  const frame = Buffer.alloc(width * height * 2);
  frame.writeUInt16LE(0xf800, 0); // 红
  frame.writeUInt16LE(0x07e0, 2); // 绿
  frame.writeUInt16LE(0x001f, 4); // 蓝
  frame.writeUInt16LE(0xffff, 6); // 白
  const png = rgb565ToPng(width, height, frame);
  const { ihdr, raw } = decodePng(png);
  assert.equal(ihdr.readUInt32BE(0), width);
  assert.equal(ihdr.readUInt32BE(4), height);
  assert.equal(ihdr[8], 8); // bit depth
  assert.equal(ihdr[9], 2); // truecolor RGB
  assert.equal(raw.length, height * (1 + width * 3));
  assert.deepEqual([...raw.subarray(1, 4)], [255, 0, 0]);
  assert.deepEqual([...raw.subarray(4, 7)], [0, 255, 0]);
  assert.deepEqual([...raw.subarray(7, 10)], [0, 0, 255]);
  assert.deepEqual([...raw.subarray(10, 13)], [255, 255, 255]);
  // 每行行首 filter 字节为 0。
  for (let y = 0; y < height; y += 1) assert.equal(raw[y * (1 + width * 3)], 0);
});

test('rgb565ToPng 尊重 stride 行距（行尾填充不进像素）', () => {
  const width = 16;
  const height = 16;
  const stride = width * 2 + 4;
  const frame = Buffer.alloc(stride * height, 0xab);
  for (let y = 0; y < height; y += 1) frame.writeUInt16LE(0x001f, y * stride); // 每行首像素蓝色
  const png = rgb565ToPng(width, height, frame, stride);
  const { raw } = decodePng(png);
  const rowBytes = 1 + width * 3;
  for (let y = 0; y < height; y += 1) {
    const base = y * rowBytes;
    assert.deepEqual([...raw.subarray(base + 1, base + 4)], [0, 0, 255]);
    // 第二个像素读的是填充 0xab（十六进制）→ RGB565 值恒定，转换后不是蓝色。
    assert.notDeepEqual([...raw.subarray(base + 4, base + 7)], [0, 0, 255]);
  }
});

test('rgb565ToPng 与参数校验拒绝非法输入', () => {
  assert.throws(() => rgb565ToPng(8, 16, Buffer.alloc(8 * 16 * 2)), /宽度无效/);
  assert.throws(() => rgb565ToPng(2048, 16, Buffer.alloc(16 * 2)), /宽度无效/);
  assert.throws(() => rgb565ToPng(16, 16, Buffer.alloc(100)), /数据长度不符/);
  assert.throws(() => rgb565ToPng(16, 16, Buffer.alloc(16 * 16 * 2), 10), /行距无效/);
  assert.throws(() => validateScreenshotParams(new URLSearchParams('w=16&h=16&fmt=rgb888')), /rgb565/);
  assert.throws(() => validateScreenshotParams(new URLSearchParams('w=4&h=16')), /宽度无效/);
  const ok = validateScreenshotParams(new URLSearchParams('w=240&h=320&stride=480'));
  assert.deepEqual(ok, { width: 240, height: 320, stride: 480 });
});

test('ScreenshotStore 落盘、按时间取最新并只保留 20 张', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'shot-store-'));
  const store = createScreenshotStore(dir);
  assert.equal(store.latest(0), null);
  const first = store.save('aabbccddeeff', Buffer.from('one'));
  const second = store.save('aabbccddeeff', Buffer.from('two'));
  assert.ok(fs.existsSync(first) && fs.existsSync(second));
  const early = Number(path.basename(first).split('-')[0]) - 1;
  assert.equal(store.latest(early), second);
  for (let i = 0; i < 25; i += 1) store.save('aabbccddeeff', Buffer.from(`bulk-${i}`));
  assert.equal(fs.readdirSync(dir).filter((f) => f.endsWith('.png')).length, 20);
  // 非法 deviceId 字符被清洗。
  const weird = store.save('../../evil', Buffer.from('x'));
  assert.match(path.basename(weird), /^[0-9]+-[0-9a-f]*\.png$/);
});

test('设备端全链路：/device/screenshot 回传 RGB565 转 PNG 落盘，/api/screenshot 触发到拿图', async (t) => {
  const { loadConfig } = await import('../src/config.mjs');
  const { loadConfiguredPlugins } = await import('../src/plugins/registry.mjs');
  const { createApp } = await import('../src/server.mjs');

  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'shot-http-'));
  fs.writeFileSync(path.join(directory, 'plugin.mjs'), `export default {
    apiVersion: 1, kind: 'coding-agent', id: 'demo_shot', label: '演示',
    capabilities: { session: 'native', model: true, cancel: true, progress: true },
    probe: () => ({ available: true, reason: '测试' }),
    run: async () => ({ status: 'completed', result: 'ok' }),
  };\n`);
  const configPath = path.join(directory, 'config.json');
  fs.writeFileSync(configPath, JSON.stringify({
    projects: [{ id: 'demo', path: directory }],
    plugins: [path.join(directory, 'plugin.mjs')],
  }));
  const config = loadConfig({ VIBE_CONFIG: configPath, VIBE_STATE_DIR: path.join(directory, 'state'), VIBE_ASR_MODE: 'off' });
  config.pluginsRuntime = await loadConfiguredPlugins(config);
  config.port = await freeTcpPort();
  do { config.devicePort = await freeTcpPort(); } while (config.port === config.devicePort);
  config.discoveryPort = await freeUdpPort();
  const app = createApp(config);
  await app.listen();
  t.after(() => app.close());
  const device = `http://127.0.0.1:${config.devicePort}`;
  const desktop = `http://127.0.0.1:${config.port}`;
  const deviceId = 'aabbccddeeff';
  const nonce = 'c'.repeat(32);
  app.pairing.open();
  app.pairing.request({ deviceId, deviceName: '测试', code: '123456', nonce }, '127.0.0.1', `127.0.0.1:${config.devicePort}`);
  app.pairing.decide(deviceId, nonce, 'confirm');
  const headers = { Authorization: `Bearer ${app.pairing.paired.get(deviceId).token}` };

  // 板子上报能力清单，声明 debug.screenshot。
  const catalog = await (await fetch(`${device}/device/capabilities`, {
    method: 'POST', headers,
    body: JSON.stringify({ events: [], commands: [{ name: 'debug.screenshot' }] }),
  })).json();
  assert.deepEqual(catalog, { events: [] });

  // 桌面触发（不 await），随后扮演固件：拉命令 → 回传帧。
  const session = await (await fetch(`${desktop}/api/session`)).json();
  const pending = fetch(`${desktop}/api/screenshot`, { method: 'POST', headers: { 'x-vibe-token': session.token } });
  await new Promise((resolve) => setTimeout(resolve, 200));

  const queued = await (await fetch(`${device}/device/commands`, { headers })).json();
  assert.ok(queued.commands.some((command) => command.name === 'debug.screenshot'), '命令应下发到板子');

  const width = 16;
  const height = 16;
  const frame = Buffer.alloc(width * height * 2, 0x00);
  frame.writeUInt16LE(0xf800, 0);
  const upload = await fetch(`${device}/device/screenshot?w=${width}&h=${height}`, {
    method: 'POST', headers, body: frame,
  });
  assert.equal(upload.status, 201);
  const savedName = (await upload.json()).saved;

  const response = await pending;
  assert.equal(response.status, 200);
  assert.equal(response.headers.get('x-screenshot-file'), savedName);
  const png = Buffer.from(await response.arrayBuffer());
  const { ihdr } = decodePng(png);
  assert.equal(ihdr.readUInt32BE(0), width);
  const stored = path.join(directory, 'state', 'screenshots', savedName);
  assert.ok(fs.existsSync(stored), '截图应落盘');

  // 数据长度不符 → 400。
  assert.equal((await fetch(`${device}/device/screenshot?w=${width}&h=${height}`, {
    method: 'POST', headers, body: Buffer.alloc(10),
  })).status, 400);

  // 未上报能力清单的设备（catalog 移除后）触发直接报错，不挂 15 秒。
  await (await fetch(`${device}/device/capabilities`, {
    method: 'POST', headers,
    body: JSON.stringify({ events: [], commands: [] }),
  })).json();
  const refused = await fetch(`${desktop}/api/screenshot`, { method: 'POST', headers: { 'x-vibe-token': session.token } });
  assert.equal(refused.status, 400);
  assert.match((await refused.json()).error, /清单|截图|下发/);
});

async function freeTcpPort() {
  const server = net.createServer();
  const port = await new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve(server.address().port)));
  await new Promise((resolve) => server.close(resolve));
  return port;
}

async function freeUdpPort() {
  const socket = dgram.createSocket('udp4');
  const port = await new Promise((resolve) => socket.bind(0, '127.0.0.1', () => resolve(socket.address().port)));
  await new Promise((resolve) => socket.close(resolve));
  return port;
}
