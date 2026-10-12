import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { readSprite, spriteChunk, SPRITE_MAX_TOTAL_BYTES, SPRITE_MAX_CHUNK } from '../src/agent-sprites.mjs';

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
