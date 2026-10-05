import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import net from 'node:net';
import { createHmac } from 'node:crypto';
import { createApp } from '../src/server.mjs';
import { UsbReceiverProtocol, forwardUsbRequest, parseMacUsbSerialPorts, linuxUsbSerialPorts } from '../src/usb-receiver.mjs';

const HELLO = { type: 'hello', version: 1, ssid: 'Vibe-Receiver', password: 'private-passphrase' };
const DIRECT_HELLO = { type: 'hello', version: 1, mode: 'direct',
  deviceId: '0123456789ab', deviceName: 'ESP32-S3 Round Display' };
const frame = (value) => Buffer.from(`${JSON.stringify(value)}\n`);

function fixture(forward = async () => ({ status: 200, contentType: 'application/json', body: Buffer.from('{}') })) {
  const sent = [];
  const receivedHello = [];
  let calls = 0;
  const protocol = new UsbReceiverProtocol({
    sendLine: async (line) => {
      const parsed = JSON.parse(line);
      sent.push(parsed);
      if (parsed.type === 'data') protocol.feed(frame({ type: 'ack', id: parsed.id }));
    },
    forward: async (...args) => { calls += 1; return forward(...args); },
    onHello: (value) => receivedHello.push(value),
  });
  return { protocol, sent, receivedHello, calls: () => calls };
}

async function response(fixture) {
  await fixture.protocol.pendingWork;
  const first = fixture.sent.find((item) => item.type === 'response');
  assert.ok(first, '应发送响应头');
  assert.equal(fixture.sent.at(-1).type, 'end');
  const body = Buffer.concat(fixture.sent.filter((item) => item.type === 'data')
    .map((item) => Buffer.from(item.chunk, 'base64')));
  assert.equal(body.length, first.length);
  return { first, body };
}

async function until(check, timeoutMs = 500) {
  const deadline = Date.now() + timeoutMs;
  while (!check()) {
    if (Date.now() >= deadline) throw new Error('等待状态超时');
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
}

test('解析分片 NDJSON，原样转发鉴权、路径与语音请求体', async () => {
  const voice = Buffer.alloc(1_100_000, 0x5a);
  let forwarded;
  const f = fixture(async (request) => {
    forwarded = request;
    return { status: 201, contentType: 'application/json; charset=utf-8', body: Buffer.from('{"ok":true}') };
  });
  const hello = frame(HELLO);
  f.protocol.feed(hello.subarray(0, 7));
  f.protocol.feed(hello.subarray(7));
  assert.deepEqual(f.receivedHello, [{ mode: 'receiver', deviceId: '', deviceName: '', ssid: HELLO.ssid, password: HELLO.password }]);
  f.protocol.feed(frame({ type: 'request', id: 7, method: 'POST',
    path: '/device/voice?sessionId=abc', authorization: `Bearer ${'a'.repeat(64)}`,
    contentType: 'audio/wav', length: voice.length }));
  assert.equal(f.protocol.current.timeoutMs, 150_000);
  for (let offset = 0; offset < voice.length; offset += 1024) {
    f.protocol.feed(frame({ type: 'data', id: 7,
      chunk: voice.subarray(offset, offset + 1024).toString('base64') }));
  }
  f.protocol.feed(frame({ type: 'end', id: 7 }));
  const result = await response(f);
  assert.equal(f.calls(), 1);
  assert.equal(forwarded.method, 'POST');
  assert.equal(forwarded.path, '/device/voice?sessionId=abc');
  assert.equal(forwarded.authorization, `Bearer ${'a'.repeat(64)}`);
  assert.equal(forwarded.contentType, 'audio/wav');
  assert.deepEqual(forwarded.body, voice);
  assert.equal(result.first.status, 201);
  assert.equal(result.body.toString(), '{"ok":true}');
  f.protocol.dispose();
});

test('直连不需要热点凭据，串口 connectionId 稳定且新连接不可复用', async () => {
  let request;
  const f = fixture(async (value) => { request = value;
    return { status: 200, contentType: 'application/json', body: Buffer.from('{}') }; });
  f.protocol.feed(frame(DIRECT_HELLO));
  f.protocol.feed(frame(DIRECT_HELLO));
  await f.protocol.pendingWork;
  assert.deepEqual(f.receivedHello[0], { mode: 'direct', deviceId: DIRECT_HELLO.deviceId,
    deviceName: DIRECT_HELLO.deviceName, ssid: '', password: '' });
  assert.equal(f.sent.length, 2);
  assert.match(f.sent[0].connectionId, /^[0-9a-f]{32}$/);
  assert.deepEqual(f.sent[1], f.sent[0]);
  f.protocol.feed(frame({ type: 'request', id: 16, method: 'GET', path: '/pair/verify?deviceId=0123456789ab', length: 0 }));
  f.protocol.feed(frame({ type: 'end', id: 16 }));
  assert.equal((await response(f)).first.status, 200);
  assert.equal(request.mode, 'direct');
  const next = fixture();
  next.protocol.feed(frame(DIRECT_HELLO));
  await next.protocol.pendingWork;
  assert.notEqual(next.sent[0].connectionId, f.sent[0].connectionId);
  f.protocol.dispose(); next.protocol.dispose();
});

test('无效直连身份和模式不能握手，已连接设备不能中途切换身份', async () => {
  const f = fixture();
  for (const change of [{ version: 2 }, { mode: 'public' }, { deviceId: '' },
    { deviceId: 'z123456789ab' }, { deviceName: 'bad\nname' }]) {
    f.protocol.feed(frame({ ...DIRECT_HELLO, ...change }));
  }
  f.protocol.feed(frame({ type: 'request', id: 17, method: 'GET', path: '/device/config', length: 0 }));
  f.protocol.feed(frame({ type: 'end', id: 17 }));
  await f.protocol.pendingWork;
  assert.equal(f.calls(), 0);
  assert.equal(f.sent.length, 0);
  f.protocol.feed(frame(DIRECT_HELLO));
  f.protocol.feed(frame(HELLO));
  f.protocol.feed(frame({ ...DIRECT_HELLO, deviceId: 'fedcba987654' }));
  await f.protocol.pendingWork;
  assert.equal(f.receivedHello.length, 1);
  f.protocol.dispose();
});

test('直连空闲握手不刷新任务在线，bye 中断语音且重新 hello 前不再接受请求', async () => {
  let refreshes = 0;
  let aborted = false;
  let byes = 0;
  const sent = [];
  const protocol = new UsbReceiverProtocol({
    sendLine: async (line) => sent.push(JSON.parse(line)),
    forward: (_request, signal) => new Promise((_resolve, reject) => {
      signal.addEventListener('abort', () => { aborted = true; reject(new Error('bye')); }, { once: true });
    }),
    onBye: (peer) => { assert.equal(peer.mode, 'direct'); byes += 1; },
    refreshAuthenticatedVoice: () => { refreshes += 1; return true; }, voiceKeepaliveMs: 10,
  });
  protocol.feed(frame(DIRECT_HELLO));
  protocol.feed(frame(DIRECT_HELLO));
  await protocol.pendingWork;
  assert.equal(refreshes, 0);
  const connectionId = sent[0].connectionId;
  protocol.feed(frame({ type: 'request', id: 18, method: 'POST', path: '/device/voice',
    authorization: 'Bearer valid', contentType: 'audio/wav', length: 0 }));
  protocol.feed(frame({ type: 'end', id: 18 }));
  await until(() => refreshes >= 2);
  protocol.feed(frame({ type: 'bye', mode: 'direct' }));
  await protocol.pendingWork;
  const stoppedAt = refreshes;
  protocol.feed(frame({ type: 'request', id: 19, method: 'POST', path: '/device/voice',
    authorization: 'Bearer valid', contentType: 'audio/wav', length: 0 }));
  await new Promise((resolve) => setTimeout(resolve, 35));
  assert.equal(refreshes, stoppedAt);
  assert.equal(aborted, true); assert.equal(byes, 1);
  assert.equal(protocol.current, null); assert.equal(protocol.responding, false);
  assert.equal(protocol.helloReceived, false);
  assert.equal(sent.filter((value) => value.type !== 'ready').length, 0);
  protocol.feed(frame(DIRECT_HELLO));
  await protocol.pendingWork;
  assert.equal(sent.at(-1).connectionId, connectionId, '退出应用后未重开串口，连接标识不变');
  protocol.dispose();
});

test('普通设备请求保留 30 秒截止，超时中断电脑请求并回 504', async () => {
  let aborted = false;
  const f = fixture((_request, signal) => new Promise((_resolve, reject) => {
    signal.addEventListener('abort', () => { aborted = true; reject(new Error('aborted')); }, { once: true });
  }));
  f.protocol.timeoutMs = 10;
  f.protocol.feed(frame(HELLO));
  f.protocol.feed(frame({ type: 'request', id: 8, method: 'GET',
    path: '/device/config', authorization: '', contentType: '', length: 0 }));
  assert.equal(f.protocol.current.timeoutMs, 10);
  f.protocol.feed(frame({ type: 'end', id: 8 }));
  await new Promise((resolve) => setTimeout(resolve, 25));
  const result = await response(f);
  assert.equal(result.first.status, 504);
  assert.equal(aborted, true);
  f.protocol.dispose();
});

test('已认证 USB 语音请求在长时间转写期间维持在线，结束后停止', async () => {
  const sent = [];
  let refreshes = 0;
  let finishForward;
  const protocol = new UsbReceiverProtocol({
    sendLine: async (line) => {
      const parsed = JSON.parse(line);
      sent.push(parsed);
      if (parsed.type === 'data') protocol.feed(frame({ type: 'ack', id: parsed.id }));
    },
    forward: () => new Promise((resolve) => { finishForward = resolve; }),
    refreshAuthenticatedVoice: (authorization) => {
      assert.equal(authorization, 'Bearer valid-token');
      refreshes += 1;
      return true;
    },
    voiceKeepaliveMs: 10,
  });
  protocol.feed(frame(HELLO));
  protocol.feed(frame({ type: 'request', id: 11, method: 'POST', path: '/device/voice?sessionId=x',
    authorization: 'Bearer valid-token', contentType: 'audio/wav', length: 0 }));
  protocol.feed(frame({ type: 'end', id: 11 }));
  await until(() => refreshes >= 3);
  assert.ok(refreshes >= 3, '转写等待期间应持续刷新在线时间');
  finishForward({ status: 201, contentType: 'application/json', body: Buffer.from('{}') });
  await protocol.pendingWork;
  assert.equal(sent.find((item) => item.type === 'response').status, 201);
  const afterFinish = refreshes;
  await new Promise((resolve) => setTimeout(resolve, 35));
  assert.equal(refreshes, afterFinish, '响应完成后应停止刷新');
  protocol.dispose();
});

test('无效令牌不维持在线，令牌撤销或 USB 断开立即停止刷新', async () => {
  let refreshes = 0;
  let valid = false;
  const protocol = new UsbReceiverProtocol({
    sendLine: async () => {},
    forward: (_request, signal) => new Promise((_resolve, reject) => {
      signal.addEventListener('abort', () => reject(new Error('disconnected')), { once: true });
    }),
    refreshAuthenticatedVoice: () => { refreshes += 1; return valid; },
    voiceKeepaliveMs: 10,
  });
  protocol.feed(frame(HELLO));
  protocol.feed(frame({ type: 'request', id: 12, method: 'POST', path: '/device/voice',
    authorization: 'Bearer invalid', contentType: 'audio/wav', length: 0 }));
  protocol.feed(frame({ type: 'end', id: 12 }));
  await new Promise((resolve) => setTimeout(resolve, 35));
  assert.equal(refreshes, 1, '无效令牌只校验一次');
  protocol.dispose();

  valid = true;
  refreshes = 0;
  const active = new UsbReceiverProtocol({
    sendLine: async () => {},
    forward: (_request, signal) => new Promise((_resolve, reject) => {
      signal.addEventListener('abort', () => reject(new Error('disconnected')), { once: true });
    }),
    refreshAuthenticatedVoice: () => { refreshes += 1; return valid; },
    voiceKeepaliveMs: 10,
  });
  active.feed(frame(HELLO));
  active.feed(frame({ type: 'request', id: 13, method: 'POST', path: '/device/voice',
    authorization: 'Bearer valid', contentType: 'audio/wav', length: 0 }));
  active.feed(frame({ type: 'end', id: 13 }));
  await until(() => refreshes >= 2);
  valid = false;
  const beforeRevoke = refreshes;
  await until(() => refreshes > beforeRevoke);
  const afterRevoke = refreshes;
  await new Promise((resolve) => setTimeout(resolve, 30));
  assert.equal(refreshes, afterRevoke, '令牌撤销后应停止刷新');
  active.dispose();
  await active.pendingWork;

  valid = true;
  refreshes = 0;
  const disconnected = new UsbReceiverProtocol({
    sendLine: async () => {},
    forward: (_request, signal) => new Promise((_resolve, reject) => {
      signal.addEventListener('abort', () => reject(new Error('disconnected')), { once: true });
    }),
    refreshAuthenticatedVoice: () => { refreshes += 1; return valid; },
    voiceKeepaliveMs: 10,
  });
  disconnected.feed(frame(HELLO));
  disconnected.feed(frame({ type: 'request', id: 14, method: 'POST', path: '/device/voice',
    authorization: 'Bearer valid', contentType: 'audio/wav', length: 0 }));
  disconnected.feed(frame({ type: 'end', id: 14 }));
  await until(() => refreshes >= 2);
  disconnected.dispose();
  await disconnected.pendingWork;
  const afterDisconnect = refreshes;
  await new Promise((resolve) => setTimeout(resolve, 30));
  assert.equal(refreshes, afterDisconnect, 'USB 断开后应停止刷新');
});

test('圆屏退出接收端热点时取消语音请求，立即停止刷新设备在线状态', async () => {
  let refreshes = 0;
  let aborted = false;
  const sent = [];
  const protocol = new UsbReceiverProtocol({
    sendLine: async (line) => { sent.push(JSON.parse(line)); },
    forward: (_request, signal) => new Promise((_resolve, reject) => {
      signal.addEventListener('abort', () => { aborted = true; reject(new Error('cancelled')); }, { once: true });
    }),
    refreshAuthenticatedVoice: () => { refreshes += 1; return true; },
    voiceKeepaliveMs: 10,
  });
  protocol.feed(frame(HELLO));
  protocol.feed(frame({ type: 'request', id: 15, method: 'POST', path: '/device/voice',
    authorization: 'Bearer valid', contentType: 'audio/wav', length: 0 }));
  protocol.feed(frame({ type: 'end', id: 15 }));
  await until(() => refreshes >= 2);
  protocol.feed(frame({ type: 'cancel', id: 15 }));
  await protocol.pendingWork;
  const stoppedAt = refreshes;
  await new Promise((resolve) => setTimeout(resolve, 35));
  assert.equal(aborted, true);
  assert.equal(refreshes, stoppedAt);
  assert.equal(protocol.current, null);
  assert.equal(protocol.responding, false);
  assert.equal(sent.filter((item) => item.type !== 'ready').length, 0);
  protocol.dispose();
});

test('USB 响应最多超前 8 块确认', async () => {
  const sent = [];
  const protocol = new UsbReceiverProtocol({
    sendLine: async (line) => { sent.push(JSON.parse(line)); },
    forward: async () => ({ status: 200, contentType: 'application/octet-stream',
      body: Buffer.alloc(9 * 1024, 7) }),
  });
  protocol.feed(frame(HELLO));
  protocol.feed(frame({ type: 'request', id: 9, method: 'GET', path: '/api/themes/bobo/files/chime.wav',
    authorization: '', contentType: '', length: 0 }));
  protocol.feed(frame({ type: 'end', id: 9 }));
  await until(() => sent.filter((item) => item.type === 'data').length >= 8);
  await new Promise((resolve) => setTimeout(resolve, 30));
  assert.equal(sent.filter((item) => item.type === 'data').length, 8);
  protocol.feed(frame({ type: 'ack', id: 9 }));
  await until(() => sent.filter((item) => item.type === 'data').length === 9);
  for (let count = 0; count < 8; count += 1) protocol.feed(frame({ type: 'ack', id: 9 }));
  await until(() => sent.at(-1)?.type === 'end');
  const body = Buffer.concat(sent.filter((item) => item.type === 'data')
    .map((item) => Buffer.from(item.chunk, 'base64')));
  assert.equal(body.length, 9 * 1024);
  protocol.dispose();
});

test('拒绝超限录音、无效 base64、报文头注入和重叠请求', async () => {
  const oversized = fixture();
  oversized.protocol.feed(frame(HELLO));
  oversized.protocol.feed(frame({ type: 'request', id: 1, method: 'POST',
    path: '/device/voice', authorization: '', contentType: 'audio/wav', length: 1_100_001 }));
  assert.equal((await response(oversized)).first.status, 400);
  assert.equal(oversized.calls(), 0);
  oversized.protocol.dispose();

  const malformed = fixture();
  malformed.protocol.feed(frame(HELLO));
  malformed.protocol.feed(frame({ type: 'request', id: 2, method: 'POST',
    path: '/device/voice', authorization: 'Bearer bad\r\nInjected: yes',
    contentType: 'audio/wav', length: 3 }));
  assert.equal((await response(malformed)).first.status, 400);
  malformed.protocol.dispose();

  const invalidData = fixture();
  invalidData.protocol.feed(frame(HELLO));
  invalidData.protocol.feed(frame({ type: 'request', id: 3, method: 'POST',
    path: '/device/voice', authorization: '', contentType: 'audio/wav', length: 3 }));
  invalidData.protocol.feed(frame({ type: 'data', id: 3, chunk: '!!!' }));
  assert.equal((await response(invalidData)).first.status, 400);
  assert.equal(invalidData.calls(), 0);
  invalidData.protocol.dispose();

  const overlap = fixture();
  overlap.protocol.feed(frame(HELLO));
  overlap.protocol.feed(frame({ type: 'request', id: 4, method: 'GET',
    path: '/device/config', authorization: '', contentType: '', length: 0 }));
  overlap.protocol.feed(frame({ type: 'request', id: 5, method: 'GET',
    path: '/device/config', authorization: '', contentType: '', length: 0 }));
  assert.equal((await response(overlap)).first.status, 409);
  assert.equal(overlap.calls(), 0);
  overlap.protocol.dispose();
});

test('丢弃超长帧、未握手帧和断线时未完成的请求', async () => {
  const f = fixture();
  f.protocol.feed(frame({ type: 'request', id: 1, method: 'GET',
    path: '/device/config', authorization: '', contentType: '', length: 0 }));
  assert.equal(f.calls(), 0);
  f.protocol.feed(frame(HELLO));
  f.protocol.feed(frame({ type: 'request', id: 2, method: 'POST',
    path: '/device/voice', authorization: '', contentType: 'audio/wav', length: 1024 }));
  f.protocol.feed(Buffer.from('x'.repeat(2100)));
  f.protocol.feed(Buffer.from('\n'));
  assert.equal((await response(f)).first.status, 400);
  assert.equal(f.calls(), 0);
  f.protocol.dispose();

  const disconnected = fixture();
  disconnected.protocol.feed(frame(HELLO));
  disconnected.protocol.feed(frame({ type: 'request', id: 3, method: 'POST',
    path: '/device/voice', authorization: '', contentType: 'audio/wav', length: 4 }));
  disconnected.protocol.feed(frame({ type: 'data', id: 3, chunk: Buffer.from('ab').toString('base64') }));
  disconnected.protocol.dispose();
  disconnected.protocol.feed(frame({ type: 'end', id: 3 }));
  assert.equal(disconnected.calls(), 0);
  assert.equal(disconnected.sent.filter((item) => item.type !== 'ready').length, 0);
});

test('转发仅访问 loopback 设备接口，并保持圆屏看到的 Host', async (t) => {
  const server = http.createServer(async (req, res) => {
    assert.equal(req.headers.host, '192.168.4.1:8788');
    assert.equal(req.headers['x-vibe-usb-forward'], 'local-secret');
    assert.equal(req.headers.authorization, 'Bearer device-token');
    assert.equal(req.url, '/device/config');
    const chunks = [];
    for await (const chunk of req) chunks.push(chunk);
    assert.equal(Buffer.concat(chunks).toString(), '{}');
    res.writeHead(202, { 'Content-Type': 'application/json' });
    res.end('{"forwarded":true}');
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise((resolve) => server.close(resolve)));
  const result = await forwardUsbRequest({ method: 'POST', path: '/device/config',
    authorization: 'Bearer device-token', contentType: 'application/json', body: Buffer.from('{}') }, {
    devicePort: server.address().port, authority: '192.168.4.1:8788',
    forwardSecret: 'local-secret', signal: new AbortController().signal,
  });
  assert.equal(result.status, 202);
  assert.equal(result.body.toString(), '{"forwarded":true}');
});

test('本机设备接口返回超大内容时终止转发', async (t) => {
  const server = http.createServer((_req, res) => {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(Buffer.alloc(262_145, 0x20));
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise((resolve) => server.close(resolve)));
  await assert.rejects(forwardUsbRequest({ method: 'GET', path: '/device/tasks',
    authorization: '', contentType: '', body: Buffer.alloc(0) }, {
    devicePort: server.address().port, authority: '192.168.4.1:8788',
    forwardSecret: 'local-secret', signal: new AbortController().signal,
  }), /响应过大/);
});

test('USB 接收器与直连仅接受持有进程密钥的 loopback 转发，并保留 HMAC 身份校验', async (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-usb-auth-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const reserve = async () => {
    const server = net.createServer();
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
    const port = server.address().port;
    await new Promise((resolve) => server.close(resolve));
    return port;
  };
  const config = {
    port: await reserve(), devicePort: await reserve(), discoveryPort: await reserve(),
    stateDir: directory, projects: [{ id: 'demo', label: 'Demo', path: directory }],
    defaultProject: 'demo', defaultProvider: 'codex', asrMode: 'off',
    workbuddy: { allowUnscopedDispatch: false },
  };
  let tunnelState = { status: 'offline' };
  const tunnel = { summary: () => tunnelState, stop: async () => {} };
  const app = createApp(config, { tunnel, run: async () => ({ status: 'completed', result: 'done' }) });
  await app.listen();
  t.after(() => app.close());
  assert.equal(app.receiver.link, null, '未启用 USB 时不应打开串口');
  const deviceId = '0123456789ab';
  const nonce = 'a'.repeat(32);
  app.pairing.open();
  app.pairing.request({ deviceId, deviceName: '圆屏', code: '123456', nonce },
    '127.0.0.1', '192.168.4.1:8788');
  app.pairing.decide(deviceId, nonce, 'confirm');
  const verify = (secret, authority = '192.168.4.1:8788', route = `/pair/verify?deviceId=${deviceId}&nonce=${nonce}`, localAddress) => new Promise((resolve, reject) => {
    const headers = { Host: authority, 'X-Forwarded-For': '127.0.0.1' };
    if (secret) headers['X-Vibe-Usb-Forward'] = secret;
    http.get({ hostname: '127.0.0.1', port: config.devicePort, localAddress,
      path: route, headers }, async (res) => {
      const chunks = [];
      for await (const chunk of res) chunks.push(chunk);
      resolve({ status: res.statusCode, body: JSON.parse(Buffer.concat(chunks).toString()) });
    }).on('error', reject);
  });
  assert.equal((await verify('')).status, 403);
  const approved = await verify(app.receiver.forwardSecret);
  assert.equal(approved.status, 200);
  assert.equal(approved.body.authority, '192.168.4.1:8788');
  const token = app.pairing.paired.get(deviceId).token;
  for (const authority of ['192.168.4.1:8788', 'usb.vibe.local:8788']) {
    assert.equal((await verify('forged-secret', authority)).status, 403);
    assert.equal((await verify('', authority, `/pair/status?deviceId=${deviceId}&nonce=${nonce}`)).status, 403,
      '伪造 USB Host 不能读取配对令牌');
    const result = await verify(app.receiver.forwardSecret, authority);
    assert.equal(result.status, 200);
    assert.equal(result.body.authority, authority);
    const signed = ['VIBE_BRIDGE_MANUAL_V2', deviceId, nonce, app.pairing.bridgeId, authority].join('\n');
    assert.equal(result.body.mac, createHmac('sha256', Buffer.from(token, 'hex')).update(signed, 'ascii').digest('hex'));
    const pairedStatus = await verify(app.receiver.forwardSecret, authority, `/pair/status?deviceId=${deviceId}&nonce=${nonce}`);
    assert.equal(pairedStatus.body.token, token);
    tunnelState = { status: 'online', url: `https://${authority}` };
    assert.equal((await verify('', authority)).status, 403, '公网域名配置不能绕过 USB 来源校验');
    const externalAddress = Object.values(os.networkInterfaces()).flat()
      .find((item) => item.family === 'IPv4' && !item.internal)?.address;
    if (externalAddress) {
      assert.equal((await verify(app.receiver.forwardSecret, authority, undefined, externalAddress)).status, 403,
        '非 loopback 即使持有密钥、伪造转发头也不可验证 USB authority');
    }
  }
  tunnelState = { status: 'offline' };
  assert.equal((await verify(app.receiver.forwardSecret, 'attacker.example:8788')).status, 403,
    '正确 USB 密钥不能授权其它 authority');
  app.receiver.state = { status: 'connected', port: '/dev/cu.usbmodem123',
    ssid: HELLO.ssid, password: HELLO.password, lastSeen: new Date().toISOString(), error: '' };
  const getJson = (port, route, host, authorization = '') => new Promise((resolve, reject) => {
    const headers = { Host: host };
    if (authorization) headers.Authorization = authorization;
    http.get({ hostname: '127.0.0.1', port, path: route, headers }, async (res) => {
      const chunks = [];
      for await (const chunk of res) chunks.push(chunk);
      resolve(JSON.parse(Buffer.concat(chunks).toString()));
    }).on('error', reject);
  });
  const management = await getJson(config.port, '/api/state', `127.0.0.1:${config.port}`);
  assert.equal(management.receiver.password, HELLO.password);
  const paired = app.pairing.paired.get(deviceId);
  paired.lastSeen = new Date(Date.now() - 45_000).toISOString();
  assert.equal(app.pairing.summary().paired[0].online, false);
  const idleUsb = new UsbReceiverProtocol({ sendLine: async () => {}, forward: async () => {},
    refreshAuthenticatedVoice: app.receiver.refreshAuthenticatedVoice });
  idleUsb.feed(frame(DIRECT_HELLO));
  idleUsb.feed(frame(DIRECT_HELLO));
  await idleUsb.pendingWork;
  assert.equal(app.pairing.summary().paired[0].online, false, '反复 USB 握手不可更新已配对设备在线时间');
  idleUsb.dispose();
  assert.equal(app.receiver.refreshAuthenticatedVoice(`Bearer ${token}`), true);
  assert.equal(app.pairing.summary().paired[0].online, true);
  assert.equal(app.receiver.refreshAuthenticatedVoice('Bearer invalid'), false);
  const device = await getJson(config.devicePort, '/device/config',
    `127.0.0.1:${config.devicePort}`, `Bearer ${token}`);
  assert.equal(JSON.stringify(device).includes(HELLO.password), false);
});

test('macOS 自动发现仅接受原生 ESP USB 和已验证的 WCH VID/PID', () => {
  const registry = `
+-o Other@0 <class IOUSBHostDevice, id 1>
  | "idProduct" = 4097
  | "idVendor" = 12345
  +-o IOSerialBSDClient <class IOSerialBSDClient, id 2>
    | "IOCalloutDevice" = "/dev/cu.usbmodem999"
+-o USB JTAG@1 <class IOUSBHostDevice, id 3>
  | "idProduct" = 4097
  | "idVendor" = 12346
  +-o IOSerialBSDClient <class IOSerialBSDClient, id 4>
    | "IOCalloutDevice" = "/dev/cu.usbmodem123"
+-o WCH@2 <class IOUSBHostDevice, id 5>
  | "idProduct" = 21971
  | "idVendor" = 6790
  +-o IOSerialBSDClient <class IOSerialBSDClient, id 6>
    | "IOCalloutDevice" = "/dev/cu.usbmodem5B7A1277141"
+-o OtherWCH@3 <class IOUSBHostDevice, id 7>
  | "idProduct" = 29987
  | "idVendor" = 6790
  +-o IOSerialBSDClient <class IOSerialBSDClient, id 8>
    | "IOCalloutDevice" = "/dev/cu.wchusbserial999"
+-o WrongPair@4 <class IOUSBHostDevice, id 9>
  | "idProduct" = 21971
  | "idVendor" = 12346
  +-o IOSerialBSDClient <class IOSerialBSDClient, id 10>
    | "IOCalloutDevice" = "/dev/cu.usbmodem456"
`;
  assert.deepEqual(parseMacUsbSerialPorts(registry), ['/dev/cu.usbmodem123', '/dev/cu.usbmodem5B7A1277141']);
});

test('Linux 从 ttyACM 和 ttyUSB 的 sysfs 父设备核对指定 USB ID', (t) => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-usb-discovery-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const sysTtyRoot = path.join(root, 'sys/class/tty');
  const devRoot = path.join(root, 'dev');
  fs.mkdirSync(sysTtyRoot, { recursive: true });
  fs.mkdirSync(devRoot);
  const add = (name, vendor, product, hasDevice = true) => {
    const usb = path.join(root, 'sys/devices/usb', name);
    const serial = path.join(usb, 'interface/serial');
    fs.mkdirSync(serial, { recursive: true });
    fs.writeFileSync(path.join(usb, 'idVendor'), `${vendor}\n`);
    fs.writeFileSync(path.join(usb, 'idProduct'), `${product}\n`);
    fs.mkdirSync(path.join(sysTtyRoot, name));
    fs.symlinkSync(serial, path.join(sysTtyRoot, name, 'device'));
    if (hasDevice) fs.writeFileSync(path.join(devRoot, name), '');
  };
  add('ttyACM0', '303a', '1001');
  add('ttyACM1', '1a86', '55d3');
  add('ttyUSB0', '1a86', '55d3');
  add('ttyUSB1', '1a86', '7523');
  add('ttyACM2', '303a', '55d3');
  add('ttyS0', '1a86', '55d3');
  add('ttyACM3', '1a86', '55d3', false);
  assert.deepEqual(linuxUsbSerialPorts({ sysTtyRoot, devRoot }).sort(),
    ['ttyACM0', 'ttyACM1', 'ttyUSB0'].map(name => path.join(devRoot, name)));
});

test('发现候选串口后仍需有效 hello，启动日志和不完整握手不能转发请求', async () => {
  const f = fixture();
  f.protocol.feed(Buffer.from('ESP-ROM:esp32c3 boot\n'));
  for (const hello of [
    { type: 'hello', version: 2, ssid: HELLO.ssid, password: HELLO.password },
    { type: 'hello', version: 1, ssid: HELLO.ssid },
    { type: 'hello', version: 1, ssid: HELLO.ssid, password: 'short' },
  ]) {
    f.protocol.feed(frame(hello));
    f.protocol.feed(frame({ type: 'request', id: 1, method: 'GET', path: '/device/config', length: 0 }));
    f.protocol.feed(frame({ type: 'end', id: 1 }));
  }
  await f.protocol.pendingWork;
  assert.equal(f.protocol.helloReceived, false);
  assert.equal(f.receivedHello.length, 0);
  assert.equal(f.calls(), 0);
  assert.equal(f.sent.length, 0);
  f.protocol.feed(frame(HELLO));
  f.protocol.feed(frame({ type: 'request', id: 2, method: 'GET', path: '/device/config', length: 0 }));
  f.protocol.feed(frame({ type: 'end', id: 2 }));
  await response(f);
  assert.equal(f.receivedHello.length, 1);
  assert.equal(f.calls(), 1);
  f.protocol.dispose();
});
