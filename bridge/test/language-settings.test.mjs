import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import http from 'node:http';
import dgram from 'node:dgram';
import { spawnSync } from 'node:child_process';
import { validateLocale, loadLanguageSettings, saveLanguageSettings } from '../src/language-settings.mjs';
import { translateKnownMessage } from '../src/i18n.mjs';
import { createApp } from '../src/server.mjs';
import { loadConfig } from '../src/config.mjs';
import { createPluginRegistry } from '../src/plugins/registry.mjs';
import { builtinAgent } from '../src/agents/index.mjs';
import { builtinSpeechRecognizers } from '../src/speech/index.mjs';
import { UsbReceiverProtocol, forwardUsbRequest } from '../src/usb-receiver.mjs';
import openai from '../src/speech/openai.mjs';
import { runCli } from '../src/agents/shared.mjs';

function temporary(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'made-language-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return fs.realpathSync(directory);
}
async function tcp() {
  const server = net.createServer();
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
  const port = server.address().port;
  await new Promise(resolve => server.close(resolve));
  return port;
}
async function udp() {
  const socket = dgram.createSocket('udp4');
  await new Promise((resolve, reject) => { socket.once('error', reject); socket.bind(0, '127.0.0.1', resolve); });
  const port = socket.address().port;
  socket.close();
  return port;
}

test('locale only accepts zh-CN/en and persists atomically without changing live state on failure', (t) => {
  const stateDir = temporary(t);
  const config = { stateDir, locale: 'zh-CN' };
  assert.equal(loadLanguageSettings(stateDir), 'zh-CN');
  assert.equal(loadLanguageSettings(stateDir, 'en'), 'en');
  for (const locale of ['EN', 'zh', '', null, true, {}, '../en']) assert.throws(() => validateLocale(locale));
  assert.deepEqual(saveLanguageSettings(config, { locale: 'en' }), { locale: 'en' });
  assert.equal(config.locale, 'en');
  assert.equal(loadLanguageSettings(stateDir), 'en');
  const file = path.join(stateDir, 'language-settings.json');
  assert.deepEqual(JSON.parse(fs.readFileSync(file, 'utf8')), { locale: 'en' });
  assert.equal(fs.statSync(file).mode & 0o777, 0o600);
  assert.throws(() => saveLanguageSettings(config, { locale: 'zh-CN', apiKey: 'no' }));
  const rename = t.mock.method(fs, 'renameSync', () => { throw new Error('simulated write failure'); });
  assert.throws(() => saveLanguageSettings(config, { locale: 'zh-CN' }), /simulated/);
  rename.mock.restore();
  assert.equal(config.locale, 'en');
  assert.equal(loadLanguageSettings(stateDir), 'en');
  assert.deepEqual(fs.readdirSync(stateDir), ['language-settings.json']);
});

test('known-message translation preserves dynamic parameters and unknown external text', () => {
  assert.equal(translateKnownMessage('正在读取文件', 'en'), 'Reading files');
  assert.equal(translateKnownMessage('正在读取文件', 'zh-CN'), '正在读取文件');
  assert.equal(translateKnownMessage('代码目录不存在：/tmp/正在读取文件', 'en'), 'Project directory does not exist: /tmp/正在读取文件');
  assert.equal(translateKnownMessage('WorkBuddy 转交失败：原始服务端错误', 'en'), 'WorkBuddy dispatch failed: 原始服务端错误');
  assert.equal(translateKnownMessage('第三方自定义错误，请保留', 'en'), '第三方自定义错误，请保留');
  assert.equal(translateKnownMessage('语音识别服务返回 429；请检查电脑侧 URL、API Key 和模型', 'en'), 'Speech service returned 429; check the URL, API key, and model on the computer');
});

test('language API requires local authorization, persists, reaches device config, and preserves user data and secrets', async (t) => {
  const stateDir = temporary(t);
  const configPath = path.join(stateDir, 'config.json');
  fs.writeFileSync(configPath, JSON.stringify({ locale: 'zh-CN', defaultProvider: 'custom',
    projects: [{ id: 'demo', label: '正在读取文件', path: stateDir }] }));
  const custom = {
    apiVersion: 1, kind: 'coding-agent', id: 'custom', label: '新任务',
    capabilities: { session: 'native', model: false, cancel: true, progress: true },
    probe: () => ({ available: true, reason: '正在读取文件' }),
    run: async () => ({ status: 'completed', result: '正在读取文件' }),
  };
  const env = { VIBE_CONFIG: configPath, VIBE_STATE_DIR: stateDir };
  const config = loadConfig(env);
  config.pluginsRuntime = createPluginRegistry([custom, builtinAgent('qoder'), builtinAgent('workbuddy'), ...builtinSpeechRecognizers,
    { apiVersion: 1, kind: 'speech-recognizer', id: 'custom_speech', label: '新任务', fields: [],
      probe: () => ({ available: true, reason: '正在读取文件' }), transcribe: async () => '' }]);
  config.port = await tcp(); config.devicePort = await tcp(); config.discoveryPort = await udp();
  const tunnelState = { status: 'error', mode: 'ngrok', error: 'ngrok 需要先填写 Authtoken', url: null };
  const fakeTunnel = { summary: () => ({ ...tunnelState }), stop: async () => ({ ...tunnelState }) };
  const app = createApp(config, { tunnel: fakeTunnel });
  await app.listen();
  let closed = false;
  t.after(async () => { if (!closed) await app.close(); });
  const base = `http://127.0.0.1:${config.port}`;
  const token = (await (await fetch(`${base}/api/session`)).json()).token;
  const post = (endpoint, payload, auth = token, extraHeaders = {}) => fetch(`${base}${endpoint}`, {
    method: 'POST', headers: { 'content-type': 'application/json', 'x-vibe-token': auth, ...extraHeaders }, body: JSON.stringify(payload),
  });
  const state = async () => (await fetch(`${base}/api/state`)).json();
  assert.equal((await state()).locale, 'zh-CN');
  assert.equal((await post('/api/settings/language', { locale: 'en' }, '')).status, 403);
  const deniedHost = await new Promise((resolve, reject) => {
    const request = http.request(`${base}/api/settings/language`, { method: 'POST',
      headers: { Host: 'evil.invalid', 'content-type': 'application/json', 'x-vibe-token': token } }, response => { response.resume(); response.on('end', () => resolve(response.statusCode)); });
    request.on('error', reject); request.end(JSON.stringify({ locale: 'en' }));
  });
  assert.equal(deniedHost, 403);
  assert.equal(config.locale, 'zh-CN');
  for (const bad of [{ locale: 'EN' }, { locale: null }, {}, { locale: 'en', secret: 'no' }, []]) {
    assert.equal((await post('/api/settings/language', bad)).status, 400);
  }
  assert.equal(config.locale, 'zh-CN');
  const voiceSettings = { url: 'https://api.siliconflow.cn/v1/audio/transcriptions', model: 'FunAudioLLM/SenseVoiceSmall', apiKey: 'test-language-private-key' };
  assert.equal((await post('/api/settings/voice', voiceSettings)).status, 200);
  const fresh = app.store.createSession({ provider: 'qoder' });
  const titled = app.store.createSession({ provider: 'custom', title: '新任务' });
  const job = app.store.submit({ sessionId: titled.id, instruction: '正在读取文件' });
  job.progress = '正在读取文件'; job.error = '任务不存在'; job.result = '正在读取文件';
  const builtinJob = app.store.submit({ sessionId: fresh.id, instruction: '用户输入不要翻译' });
  builtinJob.progress = '正在读取文件'; builtinJob.error = '任务已取消'; builtinJob.result = '正在读取文件';
  const empty = app.store.createSession({ provider: 'qoder' });
  const unavailable = app.store.submit({ sessionId: app.store.createSession({ provider: 'workbuddy' }).id, instruction: '原样保留' });
  app.store.save();
  const recordsBefore = ['jobs.json', 'sessions.json', 'secrets/voice-settings.json', 'secrets/openai-api-key'].map(file => fs.readFileSync(path.join(stateDir, file), 'utf8'));
  const deviceId = '010203040506', nonce = 'a'.repeat(32);
  app.pairing.open(); app.pairing.request({ deviceId, nonce, deviceName: '用户命名设备', code: '123456' }, '127.0.0.1', `127.0.0.1:${config.devicePort}`);
  app.pairing.decide(deviceId, nonce, 'confirm');
  const deviceToken = app.pairing.paired.get(deviceId).token;
  const deviceResponse = (endpoint, options = {}) => fetch(`http://127.0.0.1:${config.devicePort}${endpoint}`, { ...options, headers: { Authorization: `Bearer ${deviceToken}` } });
  const device = async (endpoint, options) => (await deviceResponse(endpoint, options)).json();
  // A device can choose English while the computer stays Chinese. Client
  // overrides apply to built-in messages, never the global locale or content.
  const deviceEnglish = await device('/device/config?uiLocale=en');
  assert.equal(deviceEnglish.locale, 'zh-CN');
  assert.match(deviceEnglish.providers.find(p => p.id === 'workbuddy').reason, /^WorkBuddy authorization/);
  assert.equal(deviceEnglish.providers.find(p => p.id === 'custom').reason, '正在读取文件');
  assert.equal((await device('/device/sessions?provider=qoder&uiLocale=en')).sessions.find(s => s.id === empty.id).title, 'New task');
  const englishTurn = (await device(`/device/tasks?provider=qoder&sessionId=${fresh.id}&uiLocale=en`)).jobs[0];
  assert.equal(englishTurn.progress, 'Reading files'); assert.equal(englishTurn.error, 'Task cancelled');
  assert.equal(englishTurn.result, '正在读取文件'); assert.equal(englishTurn.instruction, '用户输入不要翻译');
  assert.equal((await device('/device/tasks?provider=unknown&uiLocale=en')).error, 'Unknown coding assistant');
  assert.match((await device(`/device/tasks/${unavailable.id}/confirm?uiLocale=en`, { method: 'POST' })).error, /^WorkBuddy authorization/);
  assert.equal((await state()).locale, 'zh-CN');
  assert.equal(config.locale, 'zh-CN');
  assert.equal(fs.existsSync(path.join(stateDir, 'language-settings.json')), false);
  // Exercise the actual USB -> local HTTP forwarding path with the same
  // optional query. No protocol version or payload translation is required.
  const usbFrames = [];
  const usb = new UsbReceiverProtocol({ locale: () => config.locale,
    sendLine: async line => {
      const parsed = JSON.parse(line);
      usbFrames.push(parsed);
      if (parsed.type === 'data') usb.feed(Buffer.from(`${JSON.stringify({ type: 'ack', id: parsed.id })}\n`));
    },
    forward: (request, signal) => forwardUsbRequest(request, {
      devicePort: config.devicePort, authority: `127.0.0.1:${config.devicePort}`, forwardSecret: 'isolated-test', signal,
    }),
  });
  t.after(() => usb.dispose());
  const feedUsb = frame => usb.feed(Buffer.from(`${JSON.stringify(frame)}\n`));
  feedUsb({ type: 'hello', version: 1, ssid: 'Vibe-Receiver', password: 'private-passphrase' });
  feedUsb({ type: 'request', id: 1, method: 'GET', path: '/device/config?uiLocale=en',
    authorization: `Bearer ${deviceToken}`, contentType: '', length: 0 });
  feedUsb({ type: 'end', id: 1 });
  await usb.pendingWork;
  const usbConfig = JSON.parse(Buffer.concat(usbFrames.filter(f => f.type === 'data').map(f => Buffer.from(f.chunk, 'base64'))).toString());
  assert.equal(usbConfig.locale, 'zh-CN');
  assert.match(usbConfig.providers.find(p => p.id === 'workbuddy').reason, /^WorkBuddy authorization/);
  assert.equal(config.locale, 'zh-CN');
  usb.dispose();
  const response = await post('/api/settings/language', { locale: 'en' });
  assert.deepEqual(await response.json(), { locale: 'en' });
  let english = await state();
  assert.equal(english.locale, 'en');
  assert.equal(english.tunnel.error, 'Enter an ngrok Authtoken first');
  assert.match(english.providers.find(p => p.id === 'workbuddy').reason, /^WorkBuddy authorization/);
  assert.equal(english.providers.find(p => p.id === 'custom').reason, '正在读取文件');
  assert.equal(english.providers.find(p => p.id === 'custom').label, '新任务');
  assert.equal(english.voice.plugins.find(p => p.id === 'custom_speech').label, '新任务');
  assert.equal(english.voice.plugins.find(p => p.id === 'custom_speech').reason, '正在读取文件');
  assert.equal(english.projects[0].label, '正在读取文件');
  assert.equal(english.sessions.find(s => s.id === empty.id).title, 'New task');
  assert.equal(english.sessions.find(s => s.id === titled.id).title, '新任务');
  const external = english.jobs.find(j => j.id === job.id);
  assert.equal(external.instruction, '正在读取文件'); assert.equal(external.result, '正在读取文件');
  assert.equal(external.progress, '正在读取文件'); assert.equal(external.error, '任务不存在');
  const owned = english.jobs.find(j => j.id === builtinJob.id);
  assert.equal(owned.progress, 'Reading files'); assert.equal(owned.error, 'Task cancelled'); assert.equal(owned.result, '正在读取文件');
  assert.equal((await device('/device/config')).locale, 'en');
  assert.equal((await device('/device/sessions?provider=qoder')).sessions.find(s => s.id === empty.id).title, 'New task');
  assert.equal((await device(`/device/tasks?provider=qoder&sessionId=${fresh.id}`)).jobs[0].progress, 'Reading files');
  // The inverse selection must not receive already translated errors from
  // JobStore. Concurrent requests carry their own locale without mutation.
  const languageFileBefore = fs.readFileSync(path.join(stateDir, 'language-settings.json'), 'utf8');
  const [deviceChinese, stillEnglish, chineseTasks, englishTasks] = await Promise.all([
    device('/device/config?uiLocale=zh-CN'), state(),
    device(`/device/tasks?provider=qoder&sessionId=${fresh.id}&uiLocale=zh-CN`),
    device(`/device/tasks?provider=qoder&sessionId=${fresh.id}&uiLocale=en`),
  ]);
  assert.equal(deviceChinese.locale, 'en'); assert.equal(stillEnglish.locale, 'en');
  assert.match(deviceChinese.providers.find(p => p.id === 'workbuddy').reason, /^需 WorkBuddy/);
  assert.equal(chineseTasks.jobs[0].progress, '正在读取文件');
  assert.equal(englishTasks.jobs[0].progress, 'Reading files');
  assert.equal((await device('/device/sessions?provider=qoder&uiLocale=zh-CN')).sessions.find(s => s.id === empty.id).title, '新任务');
  assert.equal((await device('/device/sessions?provider=custom&uiLocale=en')).sessions.find(s => s.id === titled.id).title, '新任务');
  assert.equal((await device('/device/tasks?provider=unknown&uiLocale=zh-CN')).error, '未知编程工具');
  assert.match((await device(`/device/tasks/${unavailable.id}/confirm?uiLocale=zh-CN`, { method: 'POST' })).error, /^需 WorkBuddy/);
  const desktopUnavailable = await post(`/api/jobs/${unavailable.id}/confirm`, {});
  assert.equal(desktopUnavailable.status, 400);
  assert.match((await desktopUnavailable.json()).error, /^WorkBuddy authorization/);
  for (const query of ['uiLocale=fr', 'uiLocale=', 'uiLocale=EN', 'uiLocale=en&uiLocale=zh-CN', 'uiLocale=en&uiLocale=en']) {
    const rejected = await deviceResponse(`/device/config?${query}`);
    assert.equal(rejected.status, 400);
    assert.equal((await rejected.json()).error, 'Language must be zh-CN or en');
  }
  assert.equal(config.locale, 'en');
  assert.equal(fs.readFileSync(path.join(stateDir, 'language-settings.json'), 'utf8'), languageFileBefore);
  const invalid = await post('/api/settings/language', { locale: 'fr' });
  assert.equal((await invalid.json()).error, 'Language must be zh-CN or en');
  assert.equal(config.locale, 'en');
  assert.equal(english.voice.url, voiceSettings.url); assert.equal(english.voice.model, voiceSettings.model);
  assert.doesNotMatch(JSON.stringify(english), /test-language-private-key/);
  assert.deepEqual(['jobs.json', 'sessions.json', 'secrets/voice-settings.json', 'secrets/openai-api-key'].map(file => fs.readFileSync(path.join(stateDir, file), 'utf8')), recordsBefore);
  await app.close(); closed = true;
  const restored = loadConfig(env);
  assert.equal(restored.locale, 'en'); assert.equal(restored.openaiKey, voiceSettings.apiKey);
  restored.pluginsRuntime = config.pluginsRuntime;
  restored.port = await tcp(); restored.devicePort = await tcp(); restored.discoveryPort = await udp();
  const restarted = createApp(restored, { tunnel: fakeTunnel });
  await restarted.listen();
  try {
    const savedState = await (await fetch(`http://127.0.0.1:${restored.port}/api/state`)).json();
    assert.equal(savedState.locale, 'en');
    assert.equal(savedState.sessions.find(s => s.id === titled.id).title, '新任务');
    const savedToken = (await (await fetch(`http://127.0.0.1:${restored.port}/api/session`)).json()).token;
    await fetch(`http://127.0.0.1:${restored.port}/api/settings/language`, { method: 'POST', headers: { 'content-type': 'application/json', 'x-vibe-token': savedToken }, body: JSON.stringify({ locale: 'zh-CN' }) });
    assert.equal((await (await fetch(`http://127.0.0.1:${restored.port}/api/state`)).json()).tunnel.error, tunnelState.error);
  } finally { await restarted.close(); }
});

test('OpenAI transcription sends only file/model, regardless of interface locale', async (t) => {
  t.mock.method(globalThis, 'fetch', async (_url, options) => {
    assert.deepEqual([...options.body.keys()].sort(), ['file', 'model']);
    assert.equal(options.body.get('language'), null);
    assert.equal(options.body.get('languages[]'), null);
    assert.equal(options.body.get('prompt'), null);
    return { ok: true, json: async () => ({ text: 'Keep the English transcript 原样' }) };
  });
  for (const locale of ['en', 'zh-CN']) {
    const result = await openai.transcribe({ wav: Buffer.alloc(44), signal: new AbortController().signal,
      config: { locale }, settings: { apiKey: 'test-key', url: 'https://api.openai.com/v1/audio/transcriptions', model: 'gpt-transcribe' } });
    assert.equal(result, 'Keep the English transcript 原样');
  }
});

test('USB bridge-owned errors use current locale while forwarded response bytes remain untouched', async () => {
  let locale = 'en';
  const sent = [];
  let forwardedPath;
  const protocol = new UsbReceiverProtocol({ locale: () => locale,
    sendLine: async line => {
      const parsed = JSON.parse(line);
      sent.push(parsed);
      if (parsed.type === 'data') protocol.feed(Buffer.from(`${JSON.stringify({ type: 'ack', id: parsed.id })}\n`));
    },
    forward: async request => { forwardedPath = request.path; return { status: 200, contentType: 'application/json', body: Buffer.from('{"result":"任务不存在"}') }; },
  });
  const feed = frame => protocol.feed(Buffer.from(`${JSON.stringify(frame)}\n`));
  const response = () => Buffer.concat(sent.filter(f => f.type === 'data').map(f => Buffer.from(f.chunk, 'base64'))).toString();
  feed({ type: 'hello', version: 1, ssid: 'Vibe-Receiver', password: 'private-passphrase' });
  feed({ type: 'request', id: 1, method: 'POST', path: '/device/voice', authorization: '', contentType: 'audio/wav', length: 1_100_001 });
  await protocol.pendingWork;
  assert.equal(JSON.parse(response()).error, 'Invalid USB request');
  sent.length = 0; locale = 'zh-CN';
  feed({ type: 'request', id: 2, method: 'POST', path: '/device/voice', authorization: '', contentType: 'audio/wav', length: 1_100_001 });
  await protocol.pendingWork;
  assert.equal(JSON.parse(response()).error, 'USB 请求格式无效');
  sent.length = 0; locale = 'en';
  feed({ type: 'request', id: 3, method: 'GET', path: '/device/tasks?provider=custom&uiLocale=zh-CN', authorization: '', contentType: '', length: 0 });
  feed({ type: 'end', id: 3 });
  await protocol.pendingWork;
  assert.equal(response(), '{"result":"任务不存在"}');
  assert.equal(forwardedPath, '/device/tasks?provider=custom&uiLocale=zh-CN');
  protocol.dispose();
});

test('English CLI guard and bridge footer retain permission limits without translating user content', async (t) => {
  const directory = temporary(t);
  assert.equal(spawnSync('git', ['init', '-q', directory]).status, 0);
  let supplied;
  const adapter = {
    label: 'Test',
    command({ instruction }) { supplied = instruction; return { command: process.execPath, args: ['-e', 'process.stdout.write("正在读取文件")'] }; },
    createEvents() { let result = ''; return { write: chunk => { result += chunk; }, finish: () => ({ result, hasAssistantText: true }) }; },
  };
  const result = await runCli({ job: { instruction: '用户原始指令' }, project: { path: directory },
    config: { locale: 'en' }, signal: new AbortController().signal, onProgress() {}, onSession() {} }, adapter);
  assert.match(supplied, /^用户原始指令\n\nOnly modify files in the current repository\./);
  assert.match(supplied, /Do not commit, push, publish, or delete the repository/);
  assert.equal(result.result, '正在读取文件\n\nNo code changes detected.');
});
