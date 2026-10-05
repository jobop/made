import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { PassThrough } from 'node:stream';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import http from 'node:http';
import dgram from 'node:dgram';
import { createHmac } from 'node:crypto';
import { TunnelManager, inspectTailscaleFunnelStatus, inspectNgrokEndpointStatus } from '../src/tunnel.mjs';
import { createApp } from '../src/server.mjs';
import { PairingStore } from '../src/pairing.mjs';

function fakeChild() {
  const child = new EventEmitter();
  child.stdout = new PassThrough();
  child.stderr = new PassThrough();
  child.exitCode = null;
  child.signalCode = null;
  child.kill = (signal) => {
    child.signalCode = signal;
    queueMicrotask(() => child.emit('exit', null, signal));
    return true;
  };
  return child;
}

test('穿透只指向设备端口，截取官方临时 HTTPS 地址，关闭后清空地址', async () => {
  const child = fakeChild();
  let called;
  const tunnel = new TunnelManager({
    devicePort: 8788,
    spawnProcess(command, args, options) { called = { command, args, options }; return child; },
  });
  assert.equal((await tunnel.start()).status, 'starting');
  assert.equal(called.command, 'cloudflared');
  assert.deepEqual(called.args, ['tunnel', '--url', 'http://127.0.0.1:8788']);
  assert.deepEqual(called.options.stdio, ['ignore', 'pipe', 'pipe']);
  child.stderr.write('Your quick Tunnel has been created! Visit it at https://quiet-fox.trycloudflare.com\n');
  assert.equal(tunnel.summary().url, 'https://quiet-fox.trycloudflare.com');
  assert.equal(tunnel.summary().status, 'online');
  assert.equal(tunnel.summary().mode, 'quick');
  await tunnel.stop();
  assert.equal(tunnel.summary().status, 'off');
  assert.equal(tunnel.summary().url, null);
  assert.equal(child.signalCode, 'SIGTERM');
});

test('穿透缺少程序或意外退出时不保留已失效地址', async () => {
  const missing = fakeChild();
  const tunnel = new TunnelManager({ devicePort: 8788, spawnProcess: () => missing });
  await tunnel.start();
  const error = Object.assign(new Error('spawn cloudflared ENOENT'), { code: 'ENOENT' });
  missing.emit('error', error);
  assert.equal(tunnel.summary().status, 'error');
  assert.equal(tunnel.summary().url, null);
  assert.match(tunnel.summary().error, /安装 cloudflared/);

  const exited = fakeChild();
  const second = new TunnelManager({ devicePort: 8788, spawnProcess: () => exited });
  await second.start();
  exited.stderr.write('https://quiet-fox.trycloudflare.com');
  exited.emit('exit', 1, null);
  assert.equal(second.summary().status, 'error');
  assert.equal(second.summary().url, null);
});

test('固定域名模式保存私有令牌文件，只把文件路径交给 cloudflared', async (t) => {
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-named-tunnel-'));
  t.after(() => fs.rmSync(stateDir, { recursive: true, force: true }));
  const child = fakeChild();
  let called;
  const tunnel = new TunnelManager({
    devicePort: 8788, stateDir,
    spawnProcess(command, args) { called = { command, args }; return child; },
  });
  assert.throws(() => tunnel.configure({ mode: 'named', url: 'http://vibe.example.com', token: 'x'.repeat(40) }), /HTTPS/);
  assert.throws(() => tunnel.configure({ mode: 'named', url: 'https://localhost', token: 'x'.repeat(40) }), /HTTPS/);
  const secret = 'test-secret-cloudflare-token-0123456789';
  const saved = tunnel.configure({ mode: 'named', url: 'https://vibe.example.com', token: secret });
  assert.equal(saved.configuredUrl, 'https://vibe.example.com');
  assert.equal(saved.tokenConfigured, true);
  assert.equal(JSON.stringify(saved).includes(secret), false);
  const tokenFile = path.join(stateDir, 'tunnel-token');
  assert.equal(fs.readFileSync(tokenFile, 'utf8'), secret);
  assert.equal(fs.statSync(tokenFile).mode & 0o777, 0o600);
  assert.equal(fs.statSync(path.join(stateDir, 'tunnel-settings.json')).mode & 0o777, 0o600);
  const reloaded = new TunnelManager({ devicePort: 8788, stateDir });
  assert.equal(reloaded.summary().mode, 'named');
  assert.equal(reloaded.summary().tokenConfigured, true);
  assert.equal(reloaded.summary().status, 'off');
  assert.equal((await tunnel.start()).status, 'starting');
  assert.deepEqual(called.args, ['tunnel', 'run', '--token-file', tokenFile]);
  assert.equal(JSON.stringify(called).includes(secret), false);
  assert.throws(() => tunnel.configure({ mode: 'quick' }), /关闭当前穿透/);
  child.stderr.write('Registered tunnel connection connIndex=0\n');
  assert.equal(tunnel.summary().status, 'online');
  assert.equal(tunnel.summary().url, 'https://vibe.example.com');
  await tunnel.stop();
  assert.equal(tunnel.summary().status, 'off');
  tunnel.configure({ mode: 'quick', clearToken: true });
  assert.equal(fs.existsSync(tokenFile), false);
});

test('ngrok 只接受 Agent API 核实的 HTTPS 设备路由', () => {
  const entry = (url, target) => ({ url, upstream: { url: target } });
  const valid = { endpoints: [entry('https://vibe.ngrok.app', 'http://127.0.0.1:8788')] };
  assert.equal(inspectNgrokEndpointStatus(valid, 8788), 'https://vibe.ngrok.app');
  assert.equal(inspectNgrokEndpointStatus(valid, 8788, 'https://other.ngrok.app'), null);
  assert.equal(inspectNgrokEndpointStatus({ endpoints: [entry('https://vibe.ngrok.app', 'http://127.0.0.1:8787')] }, 8788), null);
  assert.equal(inspectNgrokEndpointStatus({ endpoints: [entry('http://vibe.ngrok.app', 'http://127.0.0.1:8788')] }, 8788), null);
  assert.equal(inspectNgrokEndpointStatus({ endpoints: [entry('https://vibe.ngrok.app:9443', 'http://127.0.0.1:8788')] }, 8788), null);
});

test('ngrok 凭据私有保存、只暴露 8788；地址由独立 Agent API 核实，关闭后清理本进程', async (t) => {
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-ngrok-tunnel-'));
  t.after(() => fs.rmSync(stateDir, { recursive: true, force: true }));
  const child = fakeChild();
  let called;
  let target = 'http://127.0.0.1:8787';
  const secret = 'ngrok-test-authtoken-0123456789';
  const tunnel = new TunnelManager({ devicePort: 8788, stateDir, pickNgrokPort: async () => 49123,
    fetchImpl: async (url) => {
      assert.equal(url, 'http://127.0.0.1:49123/api/endpoints');
      return { ok: true, status: 200, json: async () => ({ endpoints: [{ url: 'https://vibe.ngrok.app', upstream: { url: target } }] }) };
    },
    spawnProcess(command, args, options) { called = { command, args, options }; return child; },
  });
  assert.throws(() => tunnel.configure({ mode: 'ngrok', token: 'bad\nsecret' }), /Authtoken/);
  assert.throws(() => tunnel.configure({ mode: 'ngrok', url: 'http://vibe.ngrok.app', token: secret }), /HTTPS/);
  const saved = tunnel.configure({ mode: 'ngrok', url: 'https://vibe.ngrok.app', token: secret });
  assert.equal(saved.ngrokTokenConfigured, true);
  assert.equal(JSON.stringify(saved).includes(secret), false);
  assert.equal(fs.statSync(path.join(stateDir, 'ngrok-token')).mode & 0o777, 0o600);
  assert.equal(fs.statSync(path.join(stateDir, 'tunnel-settings.json')).mode & 0o777, 0o600);
  assert.equal(new TunnelManager({ devicePort: 8788, stateDir }).summary().mode, 'ngrok');
  assert.equal((await tunnel.start()).status, 'starting');
  assert.equal(called.command, 'ngrok');
  assert.deepEqual(called.args, ['http', 'http://127.0.0.1:8788', '--config', path.join(stateDir, 'ngrok-runtime.yml'),
    '--inspect=false', '--url', 'https://vibe.ngrok.app']);
  assert.deepEqual(called.options.stdio, ['ignore', 'pipe', 'pipe']);
  assert.equal(JSON.stringify(called).includes(secret), false);
  assert.equal(fs.statSync(path.join(stateDir, 'ngrok-runtime.yml')).mode & 0o777, 0o600);
  assert.match(fs.readFileSync(path.join(stateDir, 'ngrok-runtime.yml'), 'utf8'), /web_addr: 127\.0\.0\.1:49123/);
  await new Promise((resolve) => setTimeout(resolve, 1100));
  assert.equal(tunnel.summary().url, null);
  target = 'http://127.0.0.1:8788';
  await new Promise((resolve) => setTimeout(resolve, 1100));
  assert.equal(tunnel.summary().status, 'online');
  assert.equal(tunnel.summary().url, 'https://vibe.ngrok.app');
  await tunnel.stop();
  assert.equal(tunnel.summary().status, 'off');
  assert.equal(tunnel.summary().url, null);
  assert.equal(child.signalCode, 'SIGTERM');
  assert.equal(fs.existsSync(path.join(stateDir, 'ngrok-runtime.yml')), false);
  assert.equal(fs.existsSync(path.join(stateDir, 'ngrok-token')), true);
});

test('ngrok 旧 Agent API 回退仍核实目标，且不依赖其他用户隧道', async (t) => {
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-ngrok-legacy-'));
  t.after(() => fs.rmSync(stateDir, { recursive: true, force: true }));
  const child = fakeChild();
  const seen = [];
  const tunnel = new TunnelManager({ devicePort: 8788, stateDir, pickNgrokPort: async () => 49124,
    spawnProcess: () => child,
    fetchImpl: async (url) => {
      seen.push(url);
      if (url.endsWith('/api/endpoints')) return { ok: false, status: 404 };
      return { ok: true, json: async () => ({ tunnels: [
        { public_url: 'https://wrong.ngrok.app', config: { addr: '127.0.0.1:8787' } },
        { public_url: 'https://right.ngrok.app', config: { addr: '127.0.0.1:8788' } },
      ] }) };
    },
  });
  tunnel.configure({ mode: 'ngrok', token: 'ngrok-test-authtoken-0123456789' });
  await tunnel.start();
  assert.equal(await tunnel.ngrokStatus(), 'https://right.ngrok.app');
  assert.deepEqual(seen, ['http://127.0.0.1:49124/api/endpoints', 'http://127.0.0.1:49124/api/tunnels']);
  await tunnel.stop();
});

function tailscaleStatus(proxy = 'http://127.0.0.1:8788', allowed = true) {
  const hostPort = 'coding-mac.example-tailnet.ts.net:443';
  return { Foreground: { session: {
    TCP: { 443: { HTTPS: true } },
    Web: { [hostPort]: { Handlers: { '/': { Proxy: proxy } } } },
    AllowFunnel: { [hostPort]: allowed },
  } } };
}

test('Tailscale 状态只接受公开 Funnel 且精确代理设备端口', () => {
  assert.deepEqual(inspectTailscaleFunnelStatus(tailscaleStatus(), 8788),
    { url: 'https://coding-mac.example-tailnet.ts.net', portOccupied: true });
  assert.equal(inspectTailscaleFunnelStatus(tailscaleStatus('http://127.0.0.1:8787'), 8788).url, null);
  assert.equal(inspectTailscaleFunnelStatus(tailscaleStatus('http://127.0.0.1:8788', false), 8788).url, null);
  assert.equal(inspectTailscaleFunnelStatus({ TCP: { 443: { HTTPS: true } } }, 8788).portOccupied, true);
  assert.equal(inspectTailscaleFunnelStatus({ AllowFunnel: { 'node.example.ts.net:443': true } }, 8788).url, null);
});

test('Tailscale Funnel 用前台进程，只在状态确认公开后显示地址，关闭时验证路由', async (t) => {
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-tailscale-funnel-'));
  t.after(() => fs.rmSync(stateDir, { recursive: true, force: true }));
  const child = fakeChild();
  const calls = [];
  let started = false;
  const tunnel = new TunnelManager({ devicePort: 8788, stateDir,
    spawnProcess(command, args, options) {
      calls.push({ command, args, options });
      started = true;
      return child;
    },
    async runCommand(command, args) {
      calls.push({ command, args });
      return JSON.stringify(started && child.signalCode === null ? tailscaleStatus() : {});
    },
  });
  tunnel.configure({ mode: 'tailscale' });
  assert.equal(tunnel.summary().url, null);
  assert.equal((await tunnel.start()).status, 'starting');
  assert.deepEqual(calls[0].args, ['funnel', 'status', '--json']);
  assert.deepEqual(calls[1].args, ['funnel', '--https=443', 'http://127.0.0.1:8788']);
  assert.equal(calls[1].command, 'tailscale');
  assert.equal(calls[1].args.includes('--bg'), false);
  assert.deepEqual(calls[1].options.stdio, ['ignore', 'pipe', 'pipe']);
  child.stdout.write('Available within your tailnet: https://coding-mac.example-tailnet.ts.net\n');
  assert.equal(tunnel.summary().url, null);
  await new Promise((resolve) => setTimeout(resolve, 1100));
  assert.equal(tunnel.summary().status, 'online');
  assert.equal(tunnel.summary().url, 'https://coding-mac.example-tailnet.ts.net');
  assert.equal((await tunnel.stop()).status, 'off');
  assert.equal(child.signalCode, 'SIGTERM');
  assert.equal(tunnel.summary().url, null);
});

test('Tailscale 443 已有服务时拒绝覆盖；残留公网路由关闭失败时不假报已关闭', async (t) => {
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-tailscale-conflict-'));
  t.after(() => fs.rmSync(stateDir, { recursive: true, force: true }));
  let spawnCount = 0;
  const conflict = new TunnelManager({ devicePort: 8788, stateDir,
    spawnProcess() { spawnCount += 1; return fakeChild(); },
    async runCommand() { return JSON.stringify({ TCP: { 443: { HTTPS: true } } }); },
  });
  conflict.configure({ mode: 'tailscale' });
  assert.equal((await conflict.start()).status, 'error');
  assert.match(conflict.summary().error, /已有服务/);
  assert.equal(spawnCount, 0);

  const child = fakeChild();
  let active = false;
  const commands = [];
  const tunnel = new TunnelManager({ devicePort: 8788, stateDir,
    spawnProcess() { active = true; return child; },
    async runCommand(command, args) {
      commands.push(args);
      if (args.at(-1) === 'off') throw new Error('permission denied');
      return JSON.stringify(active ? tailscaleStatus() : {});
    },
  });
  tunnel.configure({ mode: 'tailscale' });
  await tunnel.start();
  assert.equal((await tunnel.stop()).status, 'error');
  assert.match(tunnel.summary().error, /无法确认/);
  assert.deepEqual(commands.at(-2), ['funnel', 'status', '--json']);
  assert.deepEqual(commands.at(-1), ['funnel', '--https=443', 'off']);
});

test('桥接器重启后只允许显式清理精确指向圆屏端口的残留 Funnel', async (t) => {
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-tailscale-recover-'));
  t.after(() => fs.rmSync(stateDir, { recursive: true, force: true }));
  const commands = [];
  let stale = true;
  const tunnel = new TunnelManager({ devicePort: 8788, stateDir,
    spawnProcess() { throw new Error('不应重新启动 Funnel'); },
    async runCommand(command, args) {
      commands.push(args);
      if (args.at(-1) === 'off') { stale = false; return ''; }
      return JSON.stringify(stale ? tailscaleStatus() : {});
    },
  });
  tunnel.configure({ mode: 'tailscale' });
  assert.equal((await tunnel.start()).status, 'error');
  assert.equal(tunnel.summary().cleanupAvailable, true);
  assert.equal(tunnel.summary().url, null);
  assert.equal((await tunnel.stop()).status, 'off');
  assert.equal(tunnel.summary().cleanupAvailable, false);
  assert.deepEqual(commands.at(-2), ['funnel', '--https=443', 'off']);
  assert.deepEqual(commands.at(-1), ['funnel', 'status', '--json']);
});

async function freeTcpPort() {
  const server = net.createServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const value = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return value;
}

async function freeUdpPort() {
  const socket = dgram.createSocket('udp4');
  await new Promise((resolve) => socket.bind(0, '127.0.0.1', resolve));
  const value = socket.address().port;
  socket.close();
  return value;
}

function verifyWithHost(port, host, deviceId, nonce) {
  return new Promise((resolve, reject) => {
    const request = http.request({ hostname: '127.0.0.1', port,
      path: `/pair/verify?deviceId=${deviceId}&nonce=${nonce}`, headers: { Host: host } }, (response) => {
      let raw = '';
      response.setEncoding('utf8');
      response.on('data', (chunk) => { raw += chunk; });
      response.on('end', () => resolve({ status: response.statusCode, data: JSON.parse(raw) }));
    });
    request.once('error', reject);
    request.end();
  });
}

test('公网开关只在本机桌面 API 且持有本机操作令牌时可用', async (t) => {
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-tunnel-api-'));
  const child = fakeChild();
  const tunnel = new TunnelManager({ devicePort: 8788, stateDir, spawnProcess: () => child });
  const config = {
    stateDir, port: await freeTcpPort(), devicePort: await freeTcpPort(), discoveryPort: await freeUdpPort(),
    projects: [{ id: 'demo', label: '演示项目', path: stateDir }], defaultProject: 'demo', defaultProvider: 'codex',
    workbuddy: { allowUnscopedDispatch: false }, asrMode: 'off',
  };
  const app = createApp(config, { tunnel, run: async () => ({ status: 'completed', result: '完成' }) });
  t.after(async () => { await app.close(); fs.rmSync(stateDir, { recursive: true, force: true }); });
  await app.listen();
  const request = (pathname, method = 'GET', token = '') => fetch(`http://127.0.0.1:${config.port}${pathname}`, {
    method, headers: token ? { 'x-vibe-token': token } : {},
  });
  const initial = await (await request('/api/state')).json();
  assert.equal(initial.tunnel.status, 'off');
  assert.equal((await request('/api/tunnel/start', 'POST')).status, 403);
  const token = (await (await request('/api/session')).json()).token;
  assert.equal((await request('/api/tunnel/settings', 'POST')).status, 403);
  const settingsResponse = await fetch(`http://127.0.0.1:${config.port}/api/tunnel/settings`, {
    method: 'POST', headers: { 'x-vibe-token': token, 'content-type': 'application/json' },
    body: JSON.stringify({ mode: 'quick' }),
  });
  assert.equal(settingsResponse.status, 200);
  assert.equal((await request('/api/tunnel/start', 'POST', token)).status, 200);
  child.stderr.write('https://quiet-fox.trycloudflare.com');
  assert.equal((await (await request('/api/state')).json()).tunnel.url, 'https://quiet-fox.trycloudflare.com');
  const deviceId = '0123456789ab';
  const deviceToken = 'ab'.repeat(32);
  const nonce = '12'.repeat(16);
  app.pairing.paired.set(deviceId, { deviceId, token: deviceToken, deviceName: '测试圆屏' });
  const verified = await verifyWithHost(config.devicePort, 'quiet-fox.trycloudflare.com', deviceId, nonce);
  assert.equal(verified.status, 200);
  assert.equal(verified.data.authority, 'quiet-fox.trycloudflare.com');
  const canonical = ['VIBE_BRIDGE_MANUAL_V2', deviceId, nonce, app.pairing.bridgeId,
    'quiet-fox.trycloudflare.com'].join('\n');
  assert.equal(verified.data.mac,
    createHmac('sha256', Buffer.from(deviceToken, 'hex')).update(canonical, 'ascii').digest('hex'));
  assert.equal((await verifyWithHost(config.devicePort, 'attacker.trycloudflare.com', deviceId, nonce)).status, 403);
  assert.equal((await request('/api/tunnel/stop', 'POST', token)).status, 200);
  assert.equal((await (await request('/api/state')).json()).tunnel.status, 'off');
  // The separately exposed device port never serves the desktop management API.
  assert.equal((await fetch(`http://127.0.0.1:${config.devicePort}/api/session`)).status, 401);
});

test('花生壳只授权已核验的设备域名，映射开关与公网探针接入桌面 API', async (t) => {
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-oray-api-'));
  const config = {
    stateDir, port: await freeTcpPort(), devicePort: await freeTcpPort(), discoveryPort: await freeUdpPort(),
    projects: [{ id: 'demo', label: '演示项目', path: stateDir }], defaultProject: 'demo', defaultProvider: 'codex',
    workbuddy: { allowUnscopedDispatch: false }, asrMode: 'off',
  };
  const bridgeId = new PairingStore(stateDir).bridgeId;
  let enabled = false;
  const apiCalls = [];
  const fetchImpl = async (input, options = {}) => {
    const url = new URL(input);
    if (url.hostname === 'hsk-api.oray.com') {
      apiCalls.push({ path: url.pathname, method: options.method });
      if (url.pathname === '/openapi/v2/mapping/list') return Response.json([{
        domain: 'coding.vicp.fun', port: 443, fwtype: 3, servicehost: '127.0.0.1',
        serviceport: config.devicePort, isforbid: !enabled,
      }]);
      if (url.pathname === '/openapi/api/forward/service') return Response.json({ userid: 12345 });
      if (url.pathname === '/openapi/api/mapping/12345/forbid/off') {
        enabled = true;
        return new Response(null, { status: 204 });
      }
      if (url.pathname === '/openapi/api/mapping/12345/forbid/on') {
        enabled = false;
        return new Response(null, { status: 204 });
      }
    }
    if (url.hostname === 'coding.vicp.fun' && url.pathname === '/device/tunnel-probe') {
      return Response.json({ nonce: url.searchParams.get('nonce'), bridgeId,
        authority: 'coding.vicp.fun' });
    }
    throw new Error('Unexpected request');
  };
  const tunnel = new TunnelManager({ devicePort: config.devicePort, stateDir, bridgeId, fetchImpl });
  const app = createApp(config, { tunnel, run: async () => ({ status: 'completed', result: '完成' }) });
  t.after(async () => { await app.close(); fs.rmSync(stateDir, { recursive: true, force: true }); });
  await app.listen();
  const desktop = `http://127.0.0.1:${config.port}`;
  const session = await (await fetch(`${desktop}/api/session`)).json();
  const action = (pathname, payload = undefined) => fetch(`${desktop}${pathname}`, {
    method: 'POST', headers: { 'x-vibe-token': session.token, 'content-type': 'application/json' },
    body: JSON.stringify(payload ?? {}),
  });
  const saved = await action('/api/tunnel/settings', {
    mode: 'oray', url: 'https://coding.vicp.fun', token: 'sk_12345678901234567890',
  });
  assert.equal(saved.status, 200);
  assert.equal((await saved.json()).apiKeyConfigured, true);
  assert.equal((await action('/api/tunnel/start')).status, 200);
  assert.equal(tunnel.summary().url, 'https://coding.vicp.fun');
  assert.equal(enabled, true);
  assert.equal((await fetch(`http://127.0.0.1:${config.devicePort}/device/tunnel-probe?nonce=bad`)).status, 400);
  const nonce = 'ab'.repeat(16);
  const probe = await (await fetch(`http://127.0.0.1:${config.devicePort}/device/tunnel-probe?nonce=${nonce}`)).json();
  assert.deepEqual(probe, { nonce, bridgeId, authority: `127.0.0.1:${config.devicePort}` });
  const deviceId = '0123456789ab';
  app.pairing.paired.set(deviceId, { deviceId, token: 'ab'.repeat(32), deviceName: '测试圆屏' });
  assert.equal((await verifyWithHost(config.devicePort, 'coding.vicp.fun', deviceId, nonce)).status, 200);
  assert.equal((await verifyWithHost(config.devicePort, 'other.vicp.fun', deviceId, nonce)).status, 403);
  assert.equal((await action('/api/tunnel/stop')).status, 200);
  assert.equal(enabled, false);
  assert.equal(tunnel.summary().url, null);
  assert.ok(apiCalls.some((call) => call.path.endsWith('/forbid/off')));
  assert.ok(apiCalls.some((call) => call.path.endsWith('/forbid/on')));
});
