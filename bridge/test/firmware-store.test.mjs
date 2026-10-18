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
