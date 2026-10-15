import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import dgram from 'node:dgram';
import os from 'node:os';
import path from 'node:path';
import { readSprite, spriteChunk, SPRITE_MAX_TOTAL_BYTES, SPRITE_MAX_CHUNK } from '../src/agent-sprites.mjs';
import { testRegistry } from './helpers/fixture-agents.mjs';

const PNG_1PX = Buffer.from([
  0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
  0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
  0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
  0x42, 0x60, 0x82,
]);

function makePackage({ manifest = {}, files = {} } = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'sprite-pkg-'));
  const sprite = path.join(dir, 'sprite');
  fs.mkdirSync(sprite);
  if (manifest !== null) {
    fs.writeFileSync(path.join(sprite, 'manifest.json'), JSON.stringify(manifest));
  }
  for (const [name, data] of Object.entries(files)) {
    fs.writeFileSync(path.join(sprite, name), data);
  }
  return dir;
}

const validManifest = () => ({
  version: 1,
  canvas: { w: 360, h: 360 },
  layers: [
    { id: 'body', file: 'body.png', x: 0, y: 0, z: 0 },
    { id: 'eyes', frames: ['eyes-open.png', 'eyes-closed.png'], x: 120, y: 150, z: 1,
      blink: { period_ms: 4000, closed_ms: 120 } },
  ],
});

test('没有 sprite/ 目录返回 null，缺失 manifest.json 视为无素材', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'sprite-none-'));
  assert.equal(readSprite(dir), null);
  assert.equal(readSprite(makePackage({ manifest: null })), null);
});

test('合法清单读出层结构、文件字节数与 sha256', () => {
  const body = Buffer.from('body-bytes');
  const dir = makePackage({
    manifest: validManifest(),
    files: { 'body.png': body, 'eyes-open.png': PNG_1PX, 'eyes-closed.png': PNG_1PX },
  });
  const sprite = readSprite(dir);
  assert.equal(sprite.version, 1);
  assert.deepEqual(sprite.canvas, { w: 360, h: 360 });
  assert.equal(sprite.layers.length, 2);
  assert.equal(sprite.files.length, 3);
  const bodyEntry = sprite.files.find((file) => file.file === 'body.png');
  assert.equal(bodyEntry.bytes, body.length);
  assert.equal(/^[0-9a-f]{64}$/.test(bodyEntry.sha256), true);
  assert.equal(sprite.dir, path.join(dir, 'sprite'));
});

test('重复读取命中 mtime 缓存，改文件后失效重算', () => {
  const dir = makePackage({ manifest: validManifest(), files: { 'body.png': PNG_1PX, 'eyes-open.png': PNG_1PX, 'eyes-closed.png': PNG_1PX } });
  const first = readSprite(dir);
  assert.equal(readSprite(dir), first);
  // 同 mtime 内内容不变才算命中：改文件并保证 mtime 前进。
  const future = new Date(Date.now() + 5000);
  fs.writeFileSync(path.join(dir, 'sprite', 'body.png'), Buffer.from('replaced'));
  fs.utimesSync(path.join(dir, 'sprite', 'body.png'), future, future);
  const second = readSprite(dir);
  assert.notEqual(second, first);
});

test('非法 manifest 与缺失素材被拒绝', () => {
  assert.throws(() => readSprite(makePackage({ manifest: { version: 0, canvas: { w: 360, h: 360 }, layers: [{ id: 'body', file: 'body.png', x: 0, y: 0, z: 0 }] } })), /version/);
  assert.throws(() => readSprite(makePackage({ manifest: { ...validManifest(), layers: [{ id: 'body', file: 'body.png', x: 400, y: 0, z: 0 }] } })), /x\/y\/z/);
  assert.throws(() => readSprite(makePackage({ manifest: validManifest(), files: { 'body.png': PNG_1PX } })), /素材不存在/);
  assert.throws(() => readSprite(makePackage({
    manifest: validManifest(),
    files: {
      'body.png': Buffer.alloc(200 * 1024),
      'eyes-open.png': Buffer.alloc(200 * 1024),
      'eyes-closed.png': Buffer.alloc(200 * 1024),
    },
  })), /512 KB/);
  assert.throws(() => readSprite(makePackage({
    manifest: validManifest(),
    files: { 'body.png': PNG_1PX, 'eyes-open.png': PNG_1PX, 'eyes-closed.png': PNG_1PX, 'evil.png': PNG_1PX },
  })), /未被 manifest 引用/);
});

test('spriteChunk 按偏移切片、收敛越界并拒绝未知文件', () => {
  const body = Buffer.alloc(500, 7);
  const dir = makePackage({ manifest: validManifest(), files: { 'body.png': body, 'eyes-open.png': PNG_1PX, 'eyes-closed.png': PNG_1PX } });
  const sprite = readSprite(dir);
  const head = spriteChunk(sprite, 'body.png', 0, 300);
  assert.equal(head.length, 300);
  assert.equal(head[0], 7);
  assert.equal(spriteChunk(sprite, 'body.png', 400, 300).length, 100);
  assert.equal(spriteChunk(sprite, 'body.png', 999).length, 0);
  assert.equal(spriteChunk(sprite, 'body.png').length, Math.min(500, SPRITE_MAX_CHUNK));
  assert.equal(spriteChunk(sprite, 'missing.png', 0), null);
  assert.equal(spriteChunk(null, 'body.png', 0), null);
});

test('设备端全链路：/device/config 暴露 sprite 清单，manifest 与分片可拉取', async (t) => {
  const { loadConfig } = await import('../src/config.mjs');
  const { loadConfiguredPlugins } = await import('../src/plugins/registry.mjs');
  const { createApp } = await import('../src/server.mjs');

  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'sprite-http-'));
  const packageDir = path.join(directory, 'demo_sprite');
  fs.mkdirSync(packageDir);
  const body = Buffer.alloc(700, 9);
  // 独立插件模块 + sprite/（直接落到包目录，不经 makePackage——makePackage 只生成 sprite/）。
  fs.writeFileSync(path.join(packageDir, 'plugin.mjs'), `export default {
    apiVersion: 1, kind: 'coding-agent', id: 'demo_sprite', label: '演示',
    capabilities: { session: 'native', model: true, cancel: true, progress: true },
    probe: () => ({ available: true, reason: '测试' }),
    run: async () => ({ status: 'completed', result: 'ok' }),
  };\n`);
  fs.writeFileSync(path.join(packageDir, 'plugin.json'), JSON.stringify({ id: 'demo_sprite' }));
  fs.mkdirSync(path.join(packageDir, 'sprite'));
  fs.writeFileSync(path.join(packageDir, 'sprite/manifest.json'), JSON.stringify(validManifest()));
  for (const [name, data] of Object.entries({ 'body.png': body, 'eyes-open.png': PNG_1PX, 'eyes-closed.png': PNG_1PX })) {
    fs.writeFileSync(path.join(packageDir, 'sprite', name), data);
  }

  const configPath = path.join(directory, 'config.json');
  fs.writeFileSync(configPath, JSON.stringify({ projects: [{ id: 'demo', path: directory }], plugins: [path.join(packageDir, 'plugin.mjs')] }));
  const config = loadConfig({ VIBE_CONFIG: configPath, VIBE_STATE_DIR: path.join(directory, 'state'), VIBE_ASR_MODE: 'off', MADE_HOME: path.join(directory, 'made') });
  // 内置编程助手已插件化：默认助手 codex 由临时 madeHome 里的插件包提供。
  const madeHome = path.join(directory, 'made');
  fs.mkdirSync(path.join(madeHome, 'plugins'), { recursive: true });
  fs.writeFileSync(path.join(madeHome, 'plugins', 'codex.mjs'), `export default {
    apiVersion: 1, kind: 'coding-agent', id: 'codex', label: 'Codex',
    capabilities: { session: 'native', model: true, cancel: true, progress: true },
    probe: () => ({ available: true }), run: async () => ({ status: 'completed', result: '完成' })
  };`);
  config.pluginsRuntime = await loadConfiguredPlugins(config);
  config.port = await freeTcpPort();
  do { config.devicePort = await freeTcpPort(); } while (config.port === config.devicePort);
  config.discoveryPort = await freeUdpPort();
  const app = createApp(config);
  await app.listen();
  t.after(() => app.close());
  const device = `http://127.0.0.1:${config.devicePort}`;
  const deviceId = 'aabbccddeeff';
  const nonce = 'c'.repeat(32);
  app.pairing.open();
  app.pairing.request({ deviceId, deviceName: '测试', code: '123456', nonce }, '127.0.0.1', `127.0.0.1:${config.devicePort}`);
  app.pairing.decide(deviceId, nonce, 'confirm');
  const headers = { Authorization: `Bearer ${app.pairing.paired.get(deviceId).token}` };

  // 1) config 里 sprite 清单完整（回归：字段重建曾把 sprite 丢掉）。
  const cfg = await (await fetch(`${device}/device/config`, { headers })).json();
  const provider = cfg.providers.find((item) => item.id === 'demo_sprite');
  assert.ok(provider, 'config 应包含 demo_sprite');
  assert.equal(provider.sprite.version, 1);
  assert.ok(provider.sprite.files.some((file) => file.file === 'body.png' && file.bytes === 700));

  // 2) manifest.json 直接可拉。
  const manifest = await (await fetch(`${device}/device/sprite/demo_sprite/manifest.json`, { headers })).json();
  assert.equal(manifest.version, 1);
  assert.equal(manifest.layers.length, 2);

  // 3) 分片拉 body.png 重组后与清单哈希一致。
  const meta = provider.sprite.files.find((file) => file.file === 'body.png');
  const chunks = [];
  for (let offset = 0; ; offset += chunks.at(-1)?.length || 0) {
    const response = await fetch(`${device}/device/sprite/demo_sprite/body.png?offset=${offset}&len=300`, { headers });
    assert.equal(response.status, 200);
    const buffer = Buffer.from(await response.arrayBuffer());
    if (buffer.length === 0) break;
    chunks.push(buffer);
    if (buffer.length < 300) break;
  }
  const reassembled = Buffer.concat(chunks);
  const { createHash } = await import('node:crypto');
  assert.equal(createHash('sha256').update(reassembled).digest('hex'), meta.sha256);
  assert.equal(reassembled.length, 700);

  // 4) 未知文件与未知助手都是 404。
  assert.equal((await fetch(`${device}/device/sprite/demo_sprite/nope.png`, { headers })).status, 404);
  assert.equal((await fetch(`${device}/device/sprite/other_agent/manifest.json`, { headers })).status, 404);
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
