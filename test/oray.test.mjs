import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { OrayManager, normalizeOrayDomain } from '../src/oray.mjs';

const BRIDGE_ID = 'test-bridge-123';
const API_KEY = 'sk_test_oray_secret_0123456789';

function testDir(t) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-oray-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  return dir;
}

function mockOray({ host = '127.0.0.1', port = 8788, forbidden = true, probeBridgeId = BRIDGE_ID,
  probeAuthority = 'mybox.vicp.fun',
  accountStatus = 200 } = {}) {
  const mapping = { domain: 'mybox.vicp.fun', port: 443, fwtype: 3,
    servicehost: host, serviceport: port, isforbid: forbidden, memo: '圆屏接口' };
  const calls = [];
  const fetchImpl = async (url, options = {}) => {
    const parsed = new URL(url);
    calls.push({ url, options });
    if (parsed.hostname === 'mybox.vicp.fun') {
      const nonce = parsed.searchParams.get('nonce');
      return Response.json({ nonce, bridgeId: probeBridgeId, authority: probeAuthority });
    }
    if (parsed.pathname === '/openapi/v2/mapping/list') {
      return Response.json([mapping], { status: accountStatus });
    }
    if (parsed.pathname === '/openapi/api/forward/service') {
      return Response.json({ userid: 123456 });
    }
    if (parsed.pathname.endsWith('/forbid/off')) {
      mapping.isforbid = false;
      return new Response(null, { status: 204 });
    }
    if (parsed.pathname.endsWith('/forbid/on')) {
      mapping.isforbid = true;
      return new Response(null, { status: 204 });
    }
    throw new Error(`Unexpected request ${url}`);
  };
  return { mapping, calls, fetchImpl };
}

test('花生壳域名只接受无路径端口的 HTTPS 公网域名', () => {
  assert.equal(normalizeOrayDomain('MyBox.VICP.fun'), 'mybox.vicp.fun');
  assert.equal(normalizeOrayDomain('https://mybox.vicp.fun'), 'mybox.vicp.fun');
  for (const value of ['http://mybox.vicp.fun', 'https://mybox.vicp.fun:8788',
    'https://mybox.vicp.fun/other', 'https://mybox.vicp.fun?x=1', '127.0.0.1',
    'localhost', 'https://user:pass@mybox.vicp.fun']) {
    assert.throws(() => normalizeOrayDomain(value));
  }
});

test('API Key 只存私有文件；按显式操作启停精确目标映射，并用随机挑战验证公网地址', async (t) => {
  const stateDir = testDir(t);
  const mock = mockOray();
  const oray = new OrayManager({ devicePort: 8788, stateDir, bridgeId: BRIDGE_ID, fetchImpl: mock.fetchImpl });
  const configured = oray.configure({ domain: 'https://mybox.vicp.fun', apiKey: API_KEY });
  assert.equal(configured.apiKeyConfigured, true);
  assert.equal(configured.domain, 'mybox.vicp.fun');
  assert.equal(JSON.stringify(configured).includes(API_KEY), false);
  assert.equal(fs.readFileSync(path.join(stateDir, 'oray-apikey'), 'utf8'), API_KEY);
  assert.equal(fs.statSync(path.join(stateDir, 'oray-apikey')).mode & 0o777, 0o600);
  assert.equal(fs.statSync(path.join(stateDir, 'oray-settings.json')).mode & 0o777, 0o600);

  const started = await oray.start();
  assert.equal(started.status, 'online');
  assert.equal(started.mappingEnabled, true);
  assert.equal(started.url, 'https://mybox.vicp.fun');
  const enable = mock.calls.find((call) => call.url.endsWith('/forbid/off'));
  assert.deepEqual(JSON.parse(enable.options.body), { domain: 'mybox.vicp.fun', port: 443, fwtype: 3 });
  assert.equal(enable.options.headers.Authorization, `apikey ${API_KEY}`);
  const probe = mock.calls.find((call) => call.url.includes('/device/tunnel-probe'));
  assert.match(probe.url, /nonce=[0-9a-f]{32}$/);
  assert.equal(probe.options.redirect, 'error');
  assert.throws(() => oray.configure({ domain: 'other.vicp.fun' }), /先停用/);

  const stopped = await oray.stop();
  assert.equal(stopped.status, 'off');
  assert.equal(stopped.mappingEnabled, false);
  assert.equal(stopped.url, null);
  assert.equal(mock.mapping.isforbid, true);
  assert.ok(mock.calls.some((call) => call.url.endsWith('/forbid/on')));

  const reloaded = new OrayManager({ devicePort: 8788, stateDir, bridgeId: BRIDGE_ID,
    fetchImpl: mock.fetchImpl });
  assert.equal(reloaded.summary().status, 'off');
  assert.equal(reloaded.summary().domain, 'mybox.vicp.fun');
  reloaded.configure({ clearApiKey: true });
  assert.equal(reloaded.summary().apiKeyConfigured, false);
  assert.equal(fs.existsSync(path.join(stateDir, 'oray-apikey')), false);
});

test('绝不启停目标不是本机设备 API 的映射', async (t) => {
  for (const changed of [{ host: '127.0.0.1', port: 8787 }, { host: '192.168.1.10', port: 8788 }]) {
    const stateDir = testDir(t);
    const mock = mockOray(changed);
    const oray = new OrayManager({ devicePort: 8788, stateDir, bridgeId: BRIDGE_ID, fetchImpl: mock.fetchImpl });
    oray.configure({ domain: 'mybox.vicp.fun', apiKey: API_KEY });
    const started = await oray.start();
    assert.equal(started.status, 'error');
    assert.match(started.error, /127\.0\.0\.1:8788/);
    assert.equal(started.url, null);
    assert.equal(mock.calls.some((call) => call.url.includes('/forbid/')), false);
  }
});

test('映射已启用但公网挑战不属于本桥接器时不公布地址', async (t) => {
  const stateDir = testDir(t);
  const mock = mockOray({ forbidden: false, probeBridgeId: 'another-bridge' });
  const oray = new OrayManager({ devicePort: 8788, stateDir, bridgeId: BRIDGE_ID, fetchImpl: mock.fetchImpl });
  oray.configure({ domain: 'mybox.vicp.fun', apiKey: API_KEY });
  const result = await oray.refresh();
  assert.equal(result.status, 'error');
  assert.equal(result.mappingEnabled, true);
  assert.equal(result.url, null);
  assert.match(result.error, /花生壳客户端/);
  assert.equal(mock.calls.some((call) => call.url.includes('/forbid/')), false);
  assert.equal((await oray.stop()).status, 'off');
});

test('花生壳重写 Host 时不公布无法完成地址绑定配对的域名', async (t) => {
  const stateDir = testDir(t);
  const mock = mockOray({ forbidden: false, probeAuthority: '127.0.0.1:8788' });
  const oray = new OrayManager({ devicePort: 8788, stateDir, bridgeId: BRIDGE_ID, fetchImpl: mock.fetchImpl });
  oray.configure({ domain: 'mybox.vicp.fun', apiKey: API_KEY });
  const result = await oray.refresh();
  assert.equal(result.status, 'error');
  assert.equal(result.url, null);
  assert.equal(result.mappingEnabled, true);
});

test('API 认证失败只给安全错误，不泄露 API Key', async (t) => {
  const stateDir = testDir(t);
  const mock = mockOray({ accountStatus: 401 });
  const oray = new OrayManager({ devicePort: 8788, stateDir, bridgeId: BRIDGE_ID, fetchImpl: mock.fetchImpl });
  oray.configure({ domain: 'mybox.vicp.fun', apiKey: API_KEY });
  const result = await oray.start();
  assert.equal(result.status, 'error');
  assert.match(result.error, /API Key 无效/);
  assert.equal(JSON.stringify(result).includes(API_KEY), false);
  assert.equal(result.url, null);
});
