import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createFirmwareStore, validateOtaUrl } from '../src/firmware-store.mjs';

test('固件库：导入、指定当前版本、按偏移读取，并拒绝非法地址', () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'made-fw-'));
  const store = createFirmwareStore(directory);
  assert.throws(() => validateOtaUrl('file:///tmp/a.bin'), /http/);
  assert.equal(validateOtaUrl(''), '');
  const summary = store.importBuffer({ version: '1.2.3', bytes: Buffer.from('firmware-bytes-ok'), notes: '本地' });
  assert.equal(summary.current.version, '1.2.3');
  assert.equal(summary.current.size, 17);
  assert.equal(summary.current.sha256.length, 64);
  const offer = store.deviceOffer();
  assert.equal(offer.path, '/device/firmware/current');
  assert.equal(offer.version, '1.2.3');
  const slice = store.readCurrent(9, 5);
  assert.equal(slice.bytes.toString(), 'bytes');
  assert.equal(slice.offset, 9);
  store.setOtaUrl('http://192.168.50.75:8799/made.bin');
  assert.equal(store.summary().otaUrl, 'http://192.168.50.75:8799/made.bin');
  store.setOtaUrl('/device/firmware/current');
  assert.equal(store.summary().otaUrl, '/device/firmware/current');
  store.remove(summary.current.id);
  assert.equal(store.deviceOffer().path, '');
});

function temporaryStore(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'made-fw-download-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const store = createFirmwareStore(dir);
  store.setOtaUrl('https://example.invalid/release.bin');
  return store;
}

test('固件下载有超时信号，合法流可导入', async (t) => {
  const store = temporaryStore(t);
  const result = await store.syncFromUrl(async (_url, options) => {
    assert.ok(options.signal instanceof AbortSignal);
    return new Response(Buffer.alloc(32, 0xe9));
  });
  assert.equal(result.current.size, 32);
  assert.equal(result.current.version, 'release');
});

for (const declared of [true, false]) {
  test(`固件下载${declared ? '声明' : '未声明'}超大响应时停止读取并保留原固件`, async (t) => {
    const store = temporaryStore(t);
    const original = store.importBuffer({ version: 'original', bytes: Buffer.alloc(16) });
    let reads = 0;
    let cancelled = false;
    const stream = new ReadableStream({
      pull(controller) {
        reads++;
        controller.enqueue(new Uint8Array(1024 * 1024));
      },
      cancel() { cancelled = true; },
    }, { highWaterMark: 0 });
    await assert.rejects(store.syncFromUrl(async () => new Response(stream, {
      headers: declared ? { 'content-length': String(9 * 1024 * 1024) } : {},
    })), /8MB/);
    assert.equal(cancelled, true);
    assert.ok(reads <= (declared ? 0 : 9));
    assert.equal(store.summary().current.id, original.current.id);
    assert.equal(store.summary().items.length, 1);
  });
}

test('固件下载 HTTP 失败时释放响应流，断流时不导入残缺文件', async (t) => {
  const store = temporaryStore(t);
  let cancelled = false;
  await assert.rejects(store.syncFromUrl(async () => new Response(new ReadableStream({
    cancel() { cancelled = true; },
  }), { status: 503 })), /HTTP 503/);
  assert.equal(cancelled, true);
  await assert.rejects(store.syncFromUrl(async () => new Response(new ReadableStream({
    start(controller) { controller.error(new Error('connection lost')); },
  }))), /connection lost/);
  assert.equal(store.summary().items.length, 0);
});
