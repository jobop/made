import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import dgram from 'node:dgram';
import http from 'node:http';
import { createHmac } from 'node:crypto';
import { createApp } from '../src/server.mjs';

function sampleWav() {
  const wav = Buffer.alloc(44 + 16000);
  wav.write('RIFF', 0);
  wav.writeUInt32LE(wav.length - 8, 4);
  wav.write('WAVEfmt ', 8);
  wav.writeUInt32LE(16, 16);
  wav.writeUInt16LE(1, 20);
  wav.writeUInt16LE(1, 22);
  wav.writeUInt32LE(16000, 24);
  wav.writeUInt32LE(32000, 28);
  wav.writeUInt16LE(2, 32);
  wav.writeUInt16LE(16, 34);
  wav.write('data', 36);
  wav.writeUInt32LE(16000, 40);
  return wav;
}

async function freeTcpPort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}

async function freeUdpPort() {
  const socket = dgram.createSocket('udp4');
  await new Promise((resolve, reject) => {
    socket.once('error', reject);
    socket.bind(0, '127.0.0.1', resolve);
  });
  const port = socket.address().port;
  await new Promise((resolve) => socket.close(resolve));
  return port;
}

async function ports() {
  const port = await freeTcpPort();
  let devicePort;
  do {
    devicePort = await freeTcpPort();
  } while (devicePort === port);
  return {
    port,
    devicePort,
    discoveryPort: await freeUdpPort(),
  };
}

async function jsonRequest(port, pathname, { method = 'GET', token, body } = {}) {
  const headers = {};
  if (token) headers['x-vibe-token'] = token;
  if (body !== undefined) headers['content-type'] = 'application/json';
  const response = await fetch(`http://127.0.0.1:${port}${pathname}`, {
    method, headers, body: body === undefined ? undefined : JSON.stringify(body),
  });
  return { status: response.status, data: await response.json() };
}

async function discover(port, request = 'VIBE_DISCOVER_V1') {
  const socket = dgram.createSocket('udp4');
  try {
    await new Promise((resolve, reject) => {
      socket.once('error', reject);
      socket.bind(0, '127.0.0.1', resolve);
    });
    return await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('局域网发现超时')), 2000);
      socket.once('message', (message) => {
        clearTimeout(timeout);
        resolve(message.toString('utf8'));
      });
      socket.send(request, port, '127.0.0.1', (error) => {
        if (error) {
          clearTimeout(timeout);
          reject(error);
        }
      });
    });
  } finally {
    socket.close();
  }
}

async function discoverPaired(port, deviceId, nonce) {
  const socket = dgram.createSocket('udp4');
  try {
    await new Promise((resolve, reject) => {
      socket.once('error', reject);
      socket.bind(0, '127.0.0.1', resolve);
    });
    return await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('已配对设备发现超时')), 2000);
      socket.once('message', (message, remote) => {
        clearTimeout(timeout);
        resolve({ message: message.toString('ascii'), remote });
      });
      socket.send(`VIBE_DISCOVER_PAIRED_V2 ${deviceId} ${nonce}`, port, '127.0.0.1', (error) => {
        if (error) {
          clearTimeout(timeout);
          reject(error);
        }
      });
    });
  } finally {
    socket.close();
  }
}

function verifyPairedDiscovery(response, deviceId, nonce, token, expectedBridgeId, expectedPort) {
  const match = response.message.match(/^VIBE_BRIDGE_PAIRED_V2 (\d+) ([A-Za-z0-9_-]{1,64}) (\d+\.\d+\.\d+\.\d+) ([0-9a-f]{64})$/);
  assert.ok(match, response.message);
  assert.equal(Number(match[1]), expectedPort);
  assert.equal(match[2], expectedBridgeId);
  assert.equal(match[3], response.remote.address);
  const signed = ['VIBE_BRIDGE_PAIRED_V2', deviceId, nonce, match[1], match[2], match[3]].join('\n');
  const expected = createHmac('sha256', Buffer.from(token, 'hex')).update(signed, 'ascii').digest('hex');
  assert.equal(match[4], expected);
  return match[4];
}

test('V3 扫描列出电脑名称和配对状态，选择电脑后才产生待确认配对', async (t) => {
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-discovery-list-'));
  const config = { ...await ports(), stateDir,
    projects: [{ id: 'demo', label: 'Demo', path: stateDir }],
    defaultProject: 'demo', defaultProvider: 'codex', asrMode: 'off',
    workbuddy: { allowUnscopedDispatch: false } };
  let hostname = '  办公电脑\u0000\u202E\t MacBook  ';
  t.mock.method(os, 'hostname', () => hostname);
  const app = createApp(config, { run: async () => ({ status: 'completed', result: 'done' }) });
  await app.listen();
  t.after(async () => { await app.close(); fs.rmSync(stateDir, { recursive: true, force: true }); });
  assert.equal(app.pairing.summary().open, false);

  const announcement = JSON.parse(await discover(config.discoveryPort, 'VIBE_DISCOVER_V3'));
  assert.deepEqual(announcement, {
    type: 'VIBE_BRIDGE_V3', version: 3, port: config.devicePort,
    bridgeId: app.pairing.bridgeId, name: '办公电脑 MacBook', pairingOpen: true,
  });
  assert.equal(app.pairing.pending.size, 0, '扫描不能创建配对码或待确认请求');
  assert.equal(app.pairing.paired.size, 0);

  // Retrying a scan does not extend an already open pairing window.
  const openUntil = app.pairing.activeUntil;
  hostname = '电脑'.repeat(20);
  const longName = JSON.parse(await discover(config.discoveryPort, 'VIBE_DISCOVER_V3'));
  assert.equal(longName.name, '电脑'.repeat(10) + '电');
  assert.equal(Buffer.byteLength(longName.name), 63);
  assert.equal(app.pairing.activeUntil, openUntil);
  hostname = '\u0000\u202E\n\t';
  assert.equal(JSON.parse(await discover(config.discoveryPort, 'VIBE_DISCOVER_V3')).name, 'Vibe Bridge');
  assert.equal(app.pairing.pending.size, 0);

  const deviceId = '0123456789ab';
  const nonce = 'a'.repeat(32);
  assert.deepEqual(await jsonRequest(config.devicePort, '/pair/request', {
    method: 'POST', body: { deviceId, deviceName: '圆屏', code: '123456', nonce },
  }), { status: 202, data: { status: 'pending' } });
  assert.equal(app.pairing.summary().pending.length, 1);
  app.pairing.decide(deviceId, nonce, 'confirm');
  const token = app.pairing.paired.get(deviceId).token;

  const pairedScan = JSON.parse(await discover(config.discoveryPort, 'VIBE_DISCOVER_V3'));
  assert.equal(pairedScan.pairingOpen, false, '已有配对的电脑不会因扫描自动向新设备开放');
  assert.equal(app.pairing.summary().pending.length, 0);
  assert.equal(app.pairing.paired.size, 1);
  assert.equal((await jsonRequest(config.devicePort, '/pair/request', {
    method: 'POST', body: { deviceId: '0123456789ac', deviceName: '另一圆屏', code: '654321', nonce },
  })).status, 403);
  assert.equal(app.pairing.summary().pending.length, 0);

  // Older discovery clients and signed reconnects retain their wire format.
  assert.equal(await discover(config.discoveryPort), `VIBE_BRIDGE_V1 ${config.devicePort} ${app.pairing.bridgeId}`);
  verifyPairedDiscovery(await discoverPaired(config.discoveryPort, deviceId, nonce),
    deviceId, nonce, token, app.pairing.bridgeId, config.devicePort);
  app.pairing.open();
  assert.equal(JSON.parse(await discover(config.discoveryPort, 'VIBE_DISCOVER_V3')).pairingOpen, true);
  assert.equal(app.pairing.summary().pending.length, 0);
});

test('录音连接断开会通知识别取消，迟到的识别结果不会创建任务', async (t) => {
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-voice-disconnect-'));
  const config = { ...await ports(), stateDir,
    projects: [{ id: 'demo', label: 'Demo', path: stateDir }],
    defaultProject: 'demo', defaultProvider: 'codex', asrMode: 'off',
    workbuddy: { allowUnscopedDispatch: false } };
  let started;
  const began = new Promise((resolve) => { started = resolve; });
  let finish;
  const lateResult = new Promise((resolve) => { finish = resolve; });
  let cancelled;
  const aborted = new Promise((resolve) => { cancelled = resolve; });
  let calls = 0;
  const app = createApp(config, {
    run: async () => ({ status: 'completed', result: 'done' }),
    transcribe: async (_wav, signal) => {
      if (++calls > 1) return '新的录音可以正常识别';
      signal.addEventListener('abort', cancelled, { once: true });
      started();
      await lateResult; // Simulate a service which cannot stop its in-flight work.
      return '断开后的结果不应提交';
    },
  });
  await app.listen();
  t.after(async () => { finish(); await app.close(); fs.rmSync(stateDir, { recursive: true, force: true }); });
  const deviceId = '0123456789ab';
  const nonce = 'a'.repeat(32);
  app.pairing.open();
  app.pairing.request({ deviceId, deviceName: '圆屏', code: '123456', nonce }, '127.0.0.1', `127.0.0.1:${config.devicePort}`);
  app.pairing.decide(deviceId, nonce, 'confirm');
  const token = app.pairing.paired.get(deviceId).token;
  const session = app.store.createSession({ provider: 'codex', projectId: 'demo' });
  const route = `/device/voice?sessionId=${session.id}`;
  const headers = { Authorization: `Bearer ${token}`, 'Content-Type': 'audio/wav' };
  const request = http.request({ hostname: '127.0.0.1', port: config.devicePort,
    path: route, method: 'POST', headers });
  request.on('error', () => {});
  request.end(sampleWav());
  await began;
  request.destroy();
  await aborted;
  finish();
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(app.store.list().length, 0);
  assert.equal(app.store.sessions.get(session.id).title, '新任务');
  const next = await fetch(`http://127.0.0.1:${config.devicePort}${route}`, {
    method: 'POST', headers, body: sampleWav(),
  });
  assert.equal(next.status, 201, '取消后应释放语音识别占用');
  assert.equal((await next.json()).transcript, '新的录音可以正常识别');
});

test('首台圆屏发现电脑后自动开放配对；确认令牌跨重启有效，新设备需手动开放', async (t) => {
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-pairing-'));
  let app;
  t.after(async () => {
    if (app) await app.close();
    fs.rmSync(stateDir, { recursive: true, force: true });
  });
  const base = {
    stateDir,
    projects: [{ id: 'demo', label: '演示项目', path: stateDir }],
    defaultProject: 'demo',
    defaultProvider: 'codex',
    workbuddy: { allowUnscopedDispatch: false },
    asrMode: 'off',
  };
  const firstPorts = await ports();
  app = createApp({ ...base, ...firstPorts }, {
    run: async () => ({ status: 'completed', result: '完成' }),
    transcribe: async () => '请检查标题😔',
  });
  await app.listen();

  const beforeDiscovery = await jsonRequest(firstPorts.port, '/api/state');
  assert.equal(beforeDiscovery.data.pairing.open, false);
  assert.equal(beforeDiscovery.data.pairing.paired.length, 0);

  const announcement = await discover(firstPorts.discoveryPort);
  const match = announcement.match(/^VIBE_BRIDGE_V1 (\d+) ([A-Za-z0-9_-]{1,64})$/);
  assert.ok(match, announcement);
  assert.equal(Number(match[1]), firstPorts.devicePort);
  const bridgeId = match[2];

  const desktop = await jsonRequest(firstPorts.port, '/api/session');
  assert.equal(desktop.status, 200);
  const sessionToken = desktop.data.token;
  assert.equal((await jsonRequest(firstPorts.port, '/api/sessions', {
    method: 'POST', body: { provider: 'cursor', projectId: 'demo', title: '电脑会话' },
  })).status, 403);
  const desktopSession = await jsonRequest(firstPorts.port, '/api/sessions', {
    method: 'POST', token: sessionToken,
    body: { provider: 'cursor', projectId: 'demo', title: '电脑会话' },
  });
  assert.equal(desktopSession.status, 201);
  assert.equal(desktopSession.data.title, '电脑会话');
  assert.equal((await jsonRequest(firstPorts.port, `/api/sessions/${desktopSession.data.id}/rename`, {
    method: 'POST', token: sessionToken, body: { title: '网页会话' },
  })).data.title, '网页会话');
  assert.equal((await jsonRequest(firstPorts.port, `/api/sessions/${desktopSession.data.id}/delete`, {
    method: 'POST',
  })).status, 403);
  assert.deepEqual(await jsonRequest(firstPorts.port, `/api/sessions/${desktopSession.data.id}/delete`, {
    method: 'POST', token: sessionToken,
  }), { status: 200, data: { id: desktopSession.data.id, deleted: true } });
  assert.equal((await jsonRequest(firstPorts.port, '/api/state')).data.sessions.length, 0);
  assert.equal((await jsonRequest(firstPorts.port, '/api/jobs', {
    method: 'POST', token: sessionToken, body: { instruction: '检查页面' },
  })).status, 400);
  const initial = await jsonRequest(firstPorts.port, '/api/state');
  assert.equal(initial.data.pairing.open, true);
  assert.equal(initial.data.pairing.paired.length, 0);
  assert.ok(initial.data.pairing.openUntil - Date.now() > 100_000);
  assert.ok(initial.data.pairing.openUntil - Date.now() <= 120_000);
  assert.equal((await jsonRequest(firstPorts.devicePort, '/device/config')).status, 401);

  const deviceId = '0123456789ab';
  const nonce = '0123456789abcdef0123456789abcdef';
  const request = { deviceId, deviceName: '测试圆屏', code: '482913', nonce };
  assert.deepEqual(await jsonRequest(firstPorts.devicePort, '/pair/request', {
    method: 'POST', body: request,
  }), { status: 202, data: { status: 'pending' } });
  const pending = await jsonRequest(firstPorts.port, '/api/state');
  assert.equal(pending.data.pairing.pending[0].code, request.code);
  assert.equal(pending.data.pairing.pending[0].deviceId, deviceId);
  assert.equal(pending.data.pairing.pending[0].endpoint, `127.0.0.1:${firstPorts.devicePort}`);
  assert.equal(pending.data.pairing.pending[0].token, undefined);
  assert.deepEqual(await jsonRequest(firstPorts.devicePort,
    `/pair/status?deviceId=${deviceId}&nonce=${'0'.repeat(32)}`),
  { status: 200, data: { status: 'expired' } });
  assert.equal((await jsonRequest(firstPorts.port, `/api/pair/${deviceId}/confirm`, {
    method: 'POST', body: { nonce },
  })).status, 403);
  assert.equal((await jsonRequest(firstPorts.port, `/api/pair/${deviceId}/confirm`, {
    method: 'POST', token: sessionToken, body: { nonce: '0'.repeat(32) },
  })).status, 400);
  assert.equal((await jsonRequest(firstPorts.port, `/api/pair/${deviceId}/confirm`, {
    method: 'POST', token: sessionToken, body: { nonce },
  })).status, 200);

  const approved = await jsonRequest(firstPorts.devicePort,
    `/pair/status?deviceId=${deviceId}&nonce=${nonce}`);
  assert.equal(approved.status, 200);
  assert.equal(approved.data.status, 'approved');
  assert.match(approved.data.token, /^[0-9a-f]{64}$/);
  assert.equal(approved.data.bridgeId, bridgeId);
  assert.deepEqual(await jsonRequest(firstPorts.devicePort,
    `/pair/status?deviceId=${deviceId}&nonce=${nonce}`), approved);
  assert.equal(app.pairing.status(deviceId, nonce, '192.0.2.44').status, 'expired');
  app.pairing.close();
  assert.deepEqual(await jsonRequest(firstPorts.devicePort,
    `/pair/status?deviceId=${deviceId}&nonce=${nonce}`), approved);
  app.pairing.pending.get(deviceId).expiresAt = Date.now() - 1;
  assert.equal((await jsonRequest(firstPorts.devicePort,
    `/pair/status?deviceId=${deviceId}&nonce=${nonce}`)).data.status, 'expired');
  const deviceToken = approved.data.token;
  const manualNonce = '11111111111111111111111111111111';
  const manualVerification = await jsonRequest(firstPorts.devicePort,
    `/pair/verify?deviceId=${deviceId}&nonce=${manualNonce}`);
  assert.equal(manualVerification.status, 200);
  assert.equal(manualVerification.data.bridgeId, bridgeId);
  assert.equal(manualVerification.data.token, undefined);
  assert.equal(manualVerification.data.authority, `127.0.0.1:${firstPorts.devicePort}`);
  const manualCanonical = ['VIBE_BRIDGE_MANUAL_V2', deviceId, manualNonce, bridgeId,
    `127.0.0.1:${firstPorts.devicePort}`].join('\n');
  assert.equal(manualVerification.data.mac,
    createHmac('sha256', Buffer.from(deviceToken, 'hex')).update(manualCanonical, 'ascii').digest('hex'));
  assert.equal((await jsonRequest(firstPorts.devicePort,
    `/pair/verify?deviceId=ffffffffffff&nonce=${manualNonce}`)).status, 404);
  assert.equal((await jsonRequest(firstPorts.devicePort,
    `/pair/verify?deviceId=${deviceId}&nonce=bad`)).status, 400);
  const spoofedHostStatus = await new Promise((resolve, reject) => {
    const request = http.request({ hostname: '127.0.0.1', port: firstPorts.devicePort,
      path: `/pair/verify?deviceId=${deviceId}&nonce=${manualNonce}`,
      headers: { Host: 'attacker.example.com' } }, (response) => { response.resume(); resolve(response.statusCode); });
    request.once('error', reject);
    request.end();
  });
  assert.equal(spoofedHostStatus, 403);
  const reconnectNonce = 'abcdefabcdefabcdefabcdefabcdefab';
  const authenticatedDiscovery = await discoverPaired(firstPorts.discoveryPort, deviceId, reconnectNonce);
  const signedMac = verifyPairedDiscovery(authenticatedDiscovery, deviceId, reconnectNonce,
    deviceToken, bridgeId, firstPorts.devicePort);
  const reconnectNonce2 = '1234567890abcdef1234567890abcdef';
  const secondDiscovery = await discoverPaired(firstPorts.discoveryPort, deviceId, reconnectNonce2);
  assert.notEqual(verifyPairedDiscovery(secondDiscovery, deviceId, reconnectNonce2,
    deviceToken, bridgeId, firstPorts.devicePort), signedMac);
  const deviceConfig = await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/config`, {
    headers: { Authorization: `Bearer ${deviceToken}` },
  });
  assert.equal(deviceConfig.status, 200);
  assert.equal((await deviceConfig.json()).defaultProject, 'demo');
  assert.equal((await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/heartbeat`)).status, 401);
  app.pairing.paired.get(deviceId).lastSeen = new Date(Date.now() - 31_000).toISOString();
  assert.equal(app.pairing.summary().paired[0].online, false);
  const heartbeat = await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/heartbeat`, {
    headers: { Authorization: `Bearer ${deviceToken}` },
  });
  assert.equal(heartbeat.status, 200);
  assert.equal((await heartbeat.json()).status, 'ok');
  assert.equal(app.pairing.summary().paired[0].online, true);
  assert.ok(Date.now() - Date.parse(app.pairing.summary().paired[0].lastSeen) < 5_000);
  const deviceHeaders = { Authorization: `Bearer ${deviceToken}` };
  const deviceSessionResponse = await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/sessions?provider=codex&projectId=demo`, {
    method: 'POST', headers: deviceHeaders,
  });
  assert.equal(deviceSessionResponse.status, 201);
  const deviceSession = await deviceSessionResponse.json();
  assert.match(deviceSession.id, /^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$/);
  assert.equal(deviceSession.title, '新任务');
  const throwaway = await (await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/sessions?provider=codex`, {
    method: 'POST', headers: deviceHeaders,
  })).json();
  assert.equal((await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/sessions/${throwaway.id}/delete`, {
    method: 'POST',
  })).status, 401);
  const deleted = await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/sessions/${throwaway.id}/delete`, {
    method: 'POST', headers: deviceHeaders,
  });
  assert.equal(deleted.status, 200);
  assert.deepEqual(await deleted.json(), { id: throwaway.id, deleted: true });
  const noSessionTasks = await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/tasks?provider=codex`, {
    headers: deviceHeaders,
  });
  assert.deepEqual((await noSessionTasks.json()).jobs, []);
  const voiceWithoutSession = await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/voice?provider=codex`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/wav' }, body: sampleWav(),
  });
  assert.equal(voiceWithoutSession.status, 400);
  const voiceResponse = await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/voice?provider=codex&sessionId=${deviceSession.id}`, {
    method: 'POST', headers: { ...deviceHeaders, 'content-type': 'audio/wav' }, body: sampleWav(),
  });
  assert.equal(voiceResponse.status, 201);
  const voiceTurn = await voiceResponse.json();
  assert.equal(voiceTurn.vibeSessionId, deviceSession.id);
  assert.equal(voiceTurn.transcript, '请检查标题😔');
  const deviceSessions = await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/sessions?provider=codex`, {
    headers: deviceHeaders,
  });
  assert.equal((await deviceSessions.json()).sessions[0].title, '请检查标题');
  const voiceTasks = await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/tasks?provider=codex&sessionId=${deviceSession.id}`, {
    headers: deviceHeaders,
  });
  assert.equal((await voiceTasks.json()).jobs[0].vibeSessionId, deviceSession.id);
  const historySession = app.store.createSession({ provider: 'workbuddy', projectId: 'demo' });
  const olderTurn = app.store.submit({ instruction: '先检查标题', sessionId: historySession.id });
  olderTurn.status = 'completed';
  app.store.save();
  const newestTurn = app.store.submit({ instruction: '再检查按钮', sessionId: historySession.id });
  const latestOnly = await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/tasks?provider=workbuddy&sessionId=${historySession.id}`, {
    headers: deviceHeaders,
  });
  assert.deepEqual((await latestOnly.json()).jobs.map((job) => job.id), [newestTurn.id]);
  const progressSessionId = app.store.createSession({ provider: 'codex', projectId: 'demo' }).id;
  const progressJob = app.store.submit({ instruction: '检查演示项目的文件😔', sessionId: progressSessionId });
  progressJob.status = 'running';
  progressJob.progress = '正在读取文件';
  progressJob.result = `${'答'.repeat(600)}\n\nWorkBuddy 后续回复：好了。`;
  const tasksResponse = await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/tasks?provider=codex&sessionId=${progressSessionId}`, {
    headers: { Authorization: `Bearer ${deviceToken}` },
  });
  assert.equal(tasksResponse.status, 200);
  const deviceJob = (await tasksResponse.json()).jobs[0];
  assert.equal(deviceJob.instruction, '检查演示项目的文件');
  assert.equal(deviceJob.instructionTruncated, false);
  assert.equal(deviceJob.progress, '正在读取文件');
  assert.ok(deviceJob.result.startsWith('答'.repeat(100)));
  assert.ok(deviceJob.result.includes('\n…\n'));
  assert.ok(deviceJob.result.endsWith('WorkBuddy 后续回复：好了。'));
  assert.ok(Buffer.byteLength(deviceJob.result) <= 1200);
  assert.equal(deviceJob.resultTruncated, true);
  const longSessionId = app.store.createSession({ provider: 'codex', projectId: 'demo' }).id;
  const longInstruction = app.store.submit({ instruction: '任务'.repeat(500), sessionId: longSessionId });
  const longTasksResponse = await fetch(`http://127.0.0.1:${firstPorts.devicePort}/device/tasks?provider=codex&sessionId=${longSessionId}`, {
    headers: { Authorization: `Bearer ${deviceToken}` },
  });
  const longDeviceJob = (await longTasksResponse.json()).jobs.find((job) => job.id === longInstruction.id);
  assert.equal(longDeviceJob.instruction, '任务'.repeat(200));
  assert.equal(longDeviceJob.instructionTruncated, true);
  assert.equal(fs.existsSync(path.join(stateDir, 'paired-devices.json')), true);
  if (process.platform !== 'win32') {
    assert.equal(fs.statSync(path.join(stateDir, 'paired-devices.json')).mode & 0o777, 0o600);
  }

  await app.close();
  app = undefined;
  const secondPorts = await ports();
  app = createApp({ ...base, ...secondPorts }, { run: async () => ({ status: 'completed', result: '完成' }) });
  await app.listen();
  assert.equal((await discover(secondPorts.discoveryPort)).split(' ')[2], bridgeId);
  verifyPairedDiscovery(await discoverPaired(secondPorts.discoveryPort, deviceId, reconnectNonce2),
    deviceId, reconnectNonce2, deviceToken, bridgeId, secondPorts.devicePort);
  const restarted = await jsonRequest(secondPorts.port, '/api/state');
  assert.equal(restarted.data.pairing.open, false);
  assert.equal(restarted.data.pairing.paired.length, 1);
  const restored = await fetch(`http://127.0.0.1:${secondPorts.devicePort}/device/config`, {
    headers: { Authorization: `Bearer ${deviceToken}` },
  });
  assert.equal(restored.status, 200);

  const secondId = 'abcdef012345';
  const secondNonce = 'abcdef0123456789abcdef0123456789';
  const secondRequest = {
    deviceId: secondId, deviceName: '另一块圆屏', code: '735920', nonce: secondNonce,
  };
  const closedWindow = await jsonRequest(secondPorts.devicePort, '/pair/request', {
    method: 'POST', body: secondRequest,
  });
  assert.equal(closedWindow.status, 403);
  assert.match(closedWindow.data.error, /配对窗口未开启/);
  const secondSessionToken = (await jsonRequest(secondPorts.port, '/api/session')).data.token;
  assert.equal((await jsonRequest(secondPorts.port, '/api/pair/open', {
    method: 'POST', token: secondSessionToken,
  })).status, 200);
  assert.equal((await jsonRequest(secondPorts.devicePort, '/pair/request', {
    method: 'POST', body: secondRequest,
  })).status, 202);
  assert.equal((await jsonRequest(secondPorts.port, `/api/pair/${secondId}/reject`, {
    method: 'POST', token: secondSessionToken, body: { nonce: secondNonce },
  })).status, 200);
  assert.deepEqual(await jsonRequest(secondPorts.devicePort,
    `/pair/status?deviceId=${secondId}&nonce=${secondNonce}`),
  { status: 200, data: { status: 'denied' } });
  assert.equal((await jsonRequest(secondPorts.port, '/api/state')).data.pairing.paired.length, 1);
});
