import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import { UsbReceiver } from '../src/usb-receiver.mjs';

const supported = ['darwin', 'linux'].includes(process.platform) &&
  spawnSync('python3', ['--version']).status === 0;
const HELLO = { type: 'hello', version: 1, ssid: 'VibeReceiver-TEST00', password: 'test-only-password' };
const DIRECT_HELLO = { type: 'hello', version: 1, mode: 'direct',
  deviceId: '0123456789ab', deviceName: 'ESP32-S3 Round Display' };
const frame = (value) => `${JSON.stringify(value)}\n`;

async function until(check, timeoutMs = 5000) {
  const deadline = Date.now() + timeoutMs;
  while (!check()) {
    if (Date.now() >= deadline) throw new Error('串口测试等待超时');
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
}

async function openTerminal(t) {
  const child = spawn('python3', ['-u', fileURLToPath(new URL('./helpers/usb-pty.py', import.meta.url))],
    { stdio: ['pipe', 'pipe', 'pipe'] });
  const terminal = { port: '', frames: [], write: async (data) => {
    if (!child.stdin.write(`${JSON.stringify({ write: Buffer.from(data).toString('base64') })}\n`)) {
      await once(child.stdin, 'drain');
    }
  } };
  let stdout = '';
  let serial = '';
  let failure = '';
  child.stderr.on('data', (data) => { failure += data; });
  child.stdout.on('data', (data) => {
    stdout += data;
    let end;
    while ((end = stdout.indexOf('\n')) >= 0) {
      const item = JSON.parse(stdout.slice(0, end));
      stdout = stdout.slice(end + 1);
      if (item.port) terminal.port = item.port;
      if (item.data) {
        serial += Buffer.from(item.data, 'base64').toString('utf8');
        let newline;
        while ((newline = serial.indexOf('\n')) >= 0) {
          terminal.frames.push(JSON.parse(serial.slice(0, newline)));
          serial = serial.slice(newline + 1);
        }
      }
    }
  });
  t.after(() => { child.stdin.end(); child.kill(); });
  await until(() => { if (failure) throw new Error(failure); return terminal.port; });
  return terminal;
}

for (const hello of [HELLO, DIRECT_HELLO]) {
test(`真实串口读写（${hello.mode || 'receiver'}）：握手、1.1 MB 语音、串口重连及桥接重启`,
  { skip: !supported, timeout: 30000 }, async (t) => {
    const terminal = await openTerminal(t);
    const voice = Buffer.alloc(1_100_000, 0x5a);
    let forwards = 0;
    const server = http.createServer(async (req, res) => {
      const chunks = [];
      for await (const chunk of req) chunks.push(chunk);
      assert.equal(req.headers.host, hello.mode === 'direct' ? 'usb.vibe.local:8788' : '192.168.4.1:8788');
      assert.equal(req.headers['x-vibe-usb-forward'], 'test-forward-secret');
      if (req.url === '/device/voice') assert.deepEqual(Buffer.concat(chunks), voice);
      else assert.equal(req.url, '/device/config');
      forwards += 1;
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end('{"ok":true}');
    });
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
    t.after(() => new Promise((resolve) => server.close(resolve)));
    const options = { devicePort: server.address().port, portPath: terminal.port,
      forwardSecret: 'test-forward-secret' };
    let receiver = new UsbReceiver(options);
    t.after(async () => { await receiver.stop(); });
    receiver.start();
    await until(() => receiver.link);
    await terminal.write(frame(hello));
    await until(() => terminal.frames.some((value) => value.type === 'ready'));
    assert.equal(receiver.summary().status, 'connected');
    assert.equal(receiver.summary().mode, hello.mode || 'receiver');
    assert.equal(receiver.summary().active, true);
    const firstConnectionId = terminal.frames.find((value) => value.type === 'ready').connectionId;
    assert.match(firstConnectionId, /^[0-9a-f]{32}$/);
    await terminal.write(frame(hello));
    await until(() => terminal.frames.filter((value) => value.type === 'ready').length === 2);
    assert.equal(terminal.frames.at(-1).connectionId, firstConnectionId);
    if (hello.mode === 'direct') {
      assert.equal(receiver.summary().ssid, '');
      assert.equal(receiver.summary().password, '');
      const firstLink = receiver.link;
      await terminal.write(frame({ type: 'bye', mode: 'direct' }));
      await until(() => receiver.summary().active === false);
      assert.equal(receiver.summary().status, 'waiting');
      assert.equal(receiver.link, firstLink, '退出 Vibe 应继续被动监听，不重开串口');
      await terminal.write(frame(hello));
      await until(() => terminal.frames.filter((value) => value.type === 'ready').length === 3);
      assert.equal(terminal.frames.at(-1).connectionId, firstConnectionId);
    }
    terminal.frames.length = 0;

    const request = [frame({ type: 'request', id: 1, method: 'POST', path: '/device/voice',
      authorization: 'Bearer test-token', contentType: 'audio/wav', length: voice.length })];
    for (let offset = 0; offset < voice.length; offset += 1024) {
      request.push(frame({ type: 'data', id: 1, chunk: voice.subarray(offset, offset + 1024).toString('base64') }));
    }
    request.push(frame({ type: 'end', id: 1 }));
    await terminal.write(request.join(''));
    try {
      await until(() => terminal.frames.some((value) => value.type === 'end' && value.id === 1), 18000);
    } catch (error) {
      throw new Error(`${error.message}: ${JSON.stringify({ status: receiver.summary().status,
        error: receiver.summary().error, received: receiver.link?.protocol.current?.received,
        forwarded: forwards, responseFrames: terminal.frames.map(({ type, status }) => ({ type, status })) })}`);
    }
    assert.equal(terminal.frames.find((value) => value.type === 'response').status, 200);
    assert.equal(forwards, 1);

    receiver.disconnect(receiver.link);
    terminal.frames.length = 0;
    await receiver.poll();
    await until(() => receiver.link);
    await terminal.write(frame(hello));
    await until(() => terminal.frames.some((value) => value.type === 'ready'));
    const secondConnectionId = terminal.frames.find((value) => value.type === 'ready').connectionId;
    assert.notEqual(secondConnectionId, firstConnectionId, '重开串口必须触发圆屏重新验证电脑身份');

    await receiver.stop();
    assert.equal(receiver.summary().status, 'disconnected');
    terminal.frames.length = 0;
    receiver = new UsbReceiver(options);
    receiver.start();
    await until(() => receiver.link);
    await terminal.write(frame(hello));
    await until(() => terminal.frames.some((value) => value.type === 'ready'));
    assert.notEqual(terminal.frames.find((value) => value.type === 'ready').connectionId, secondConnectionId,
      '重建桥接对象不得复用上一次 connectionId');
    await terminal.write(frame({ type: 'request', id: 2, method: 'GET', path: '/device/config', length: 0 }) +
      frame({ type: 'end', id: 2 }));
    await until(() => terminal.frames.some((value) => value.type === 'end' && value.id === 2));
    assert.equal(forwards, 2);
  });
}

test('圆屏安静超过旧握手超时后保持被动监听，打开直连立即发现且事件循环继续响应',
  { skip: !supported, timeout: 12000 }, async (t) => {
    const terminal = await openTerminal(t);
    const receiver = new UsbReceiver({ devicePort: 1, portPath: terminal.port });
    t.after(async () => { await receiver.stop(); });
    receiver.start();
    await until(() => receiver.link);
    const quietLink = receiver.link;
    let ticks = 0;
    const timer = setInterval(() => { ticks += 1; }, 50);
    t.after(() => clearInterval(timer));
    await until(() => receiver.summary().error === 'USB 已连接，等待打开码得应用的 USB 直连', 9000);
    assert.equal(receiver.link, quietLink);
    assert.equal(receiver.summary().status, 'waiting');
    assert.equal(receiver.summary().password, '');
    assert.ok(ticks >= 100, '等待握手期间不能阻塞网页事件循环');
    const openedAt = Date.now();
    await terminal.write(frame(DIRECT_HELLO));
    await until(() => terminal.frames.some((value) => value.type === 'ready'), 1000);
    assert.ok(Date.now() - openedAt < 1000, '不能再等待旧的 60 秒端口冷却');
    assert.equal(receiver.summary().status, 'connected');
    assert.equal(receiver.link, quietLink);
  });

test('直连 bye 在真实串口上立即取消等待转写的 HTTP 请求并停止保活',
  { skip: !supported, timeout: 10000 }, async (t) => {
    const terminal = await openTerminal(t);
    let started = false;
    let closed = false;
    let refreshes = 0;
    const server = http.createServer((req, res) => {
      assert.equal(req.headers.host, 'usb.vibe.local:8788');
      started = true;
      res.on('close', () => { closed = true; });
    });
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
    t.after(() => new Promise((resolve) => server.close(resolve)));
    const receiver = new UsbReceiver({ devicePort: server.address().port, portPath: terminal.port,
      forwardSecret: 'serial-test-secret', refreshAuthenticatedVoice: () => { refreshes += 1; return true; } });
    t.after(async () => { await receiver.stop(); });
    receiver.start();
    await until(() => receiver.link);
    receiver.link.protocol.voiceKeepaliveMs = 10;
    await terminal.write(frame(DIRECT_HELLO));
    await until(() => terminal.frames.some((value) => value.type === 'ready'));
    await terminal.write(frame({ type: 'request', id: 3, method: 'POST', path: '/device/voice',
      authorization: 'Bearer test-token', contentType: 'audio/wav', length: 0 }) + frame({ type: 'end', id: 3 }));
    await until(() => started && refreshes >= 2);
    await terminal.write(frame({ type: 'bye', mode: 'direct' }));
    await until(() => closed && receiver.summary().active === false);
    const stoppedAt = refreshes;
    await new Promise((resolve) => setTimeout(resolve, 40));
    assert.equal(refreshes, stoppedAt);
    assert.equal(receiver.summary().status, 'waiting');
    assert.equal(receiver.link.protocol.current, null);
    assert.equal(receiver.link.protocol.responding, false);
    assert.equal(terminal.frames.some((value) => value.type === 'response'), false);
  });
