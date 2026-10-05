import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import dgram from 'node:dgram';
import { createApp } from '../src/server.mjs';
import { loadConfig } from '../src/config.mjs';
import { transcribe } from '../src/transcribe.mjs';
import { DEFAULT_TRANSCRIBE_URL, validateTranscribeUrl } from '../src/voice-settings.mjs';

function sampleWav() {
  const wav = Buffer.alloc(44 + 16000);
  wav.write('RIFF', 0);
  wav.writeUInt32LE(wav.length - 8, 4);
  wav.write('WAVEfmt ', 8);
  wav.writeUInt32LE(16, 16);
  wav.writeUInt16LE(1, 20);
  wav.writeUInt16LE(1, 22);
  wav.writeUInt16LE(16000, 24);
  wav.writeUInt32LE(32000, 28);
  wav.writeUInt16LE(2, 32);
  wav.writeUInt16LE(16, 34);
  wav.write('data', 36);
  wav.writeUInt32LE(16000, 40);
  return wav;
}

async function availableTcpPort() {
  const server = net.createServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}

async function availableUdpPort() {
  const socket = dgram.createSocket('udp4');
  await new Promise((resolve) => socket.bind(0, '127.0.0.1', resolve));
  const port = socket.address().port;
  await new Promise((resolve) => socket.close(resolve));
  return port;
}

test('语音 URL 只接受 HTTPS，或本机代理的 HTTP', () => {
  assert.equal(validateTranscribeUrl(DEFAULT_TRANSCRIBE_URL), DEFAULT_TRANSCRIBE_URL);
  assert.equal(validateTranscribeUrl('http://127.0.0.1:9000/v1/audio/transcriptions'), 'http://127.0.0.1:9000/v1/audio/transcriptions');
  for (const value of [
    'file:///etc/passwd', 'http://example.com/v1/audio/transcriptions',
    'http://192.168.1.1/v1/audio/transcriptions',
    'https://user:pass@example.com/v1/audio/transcriptions',
    'https://example.com/v1/audio/transcriptions#fragment',
  ]) assert.throws(() => validateTranscribeUrl(value));
});

test('第三方 OpenAI 兼容接口只收到 file 和 model', async (t) => {
  t.mock.method(globalThis, 'fetch', async (url, options) => {
    assert.equal(url, 'https://api.siliconflow.cn/v1/audio/transcriptions');
    assert.equal(options.headers.Authorization, 'Bearer local-test-key');
    assert.equal(options.redirect, 'error');
    assert.equal(options.body.get('model'), 'FunAudioLLM/SenseVoiceSmall');
    assert.equal(options.body.get('file').type, 'audio/wav');
    assert.equal(options.body.get('languages[]'), null);
    assert.equal(options.body.get('language'), null);
    assert.equal(options.body.get('prompt'), null);
    return { ok: true, json: async () => ({ text: '帮我修复按钮' }) };
  });
  assert.equal(await transcribe(sampleWav(), {
    asrMode: 'openai', openaiKey: 'local-test-key',
    transcribeUrl: 'https://api.siliconflow.cn/v1/audio/transcriptions',
    transcribeModel: 'FunAudioLLM/SenseVoiceSmall',
  }), '帮我修复按钮');
});

test('电脑端三要素设置保存到本机，密钥不会通过状态接口返回', async (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-voice-settings-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const configPath = path.join(directory, 'config.json');
  fs.writeFileSync(configPath, JSON.stringify({
    defaultProject: 'demo', defaultProvider: 'codex',
    projects: [{ id: 'demo', path: directory }],
  }));
  const env = { VIBE_CONFIG: configPath, VIBE_STATE_DIR: directory, VIBE_ASR_MODE: 'openai' };
  const config = loadConfig(env);
  config.port = await availableTcpPort();
  config.devicePort = await availableTcpPort();
  config.discoveryPort = await availableUdpPort();
  const app = createApp(config, { run: async () => ({ status: 'completed', result: '完成' }) });
  await app.listen();
  t.after(async () => app.close());
  const base = `http://127.0.0.1:${config.port}`;
  const session = await (await fetch(`${base}/api/session`)).json();
  const settingsUrl = `${base}/api/settings/voice`;
  const headers = { 'Content-Type': 'application/json', 'x-vibe-token': session.token };
  const options = (payload) => ({ method: 'POST', headers, body: JSON.stringify(payload) });
  const voice = {
    url: 'https://api.siliconflow.cn/v1/audio/transcriptions',
    model: 'FunAudioLLM/SenseVoiceSmall',
    apiKey: 'secret-test-key',
  };
  assert.equal((await fetch(settingsUrl, options(voice))).status, 200);
  let state = await (await fetch(`${base}/api/state`)).json();
  assert.equal(state.voice.url, voice.url);
  assert.equal(state.voice.model, voice.model);
  assert.equal(state.voice.available, true);
  assert.equal(state.voice.keySource, 'saved');
  assert.equal(JSON.stringify(state).includes(voice.apiKey), false);
  const secretDir = path.join(directory, 'secrets');
  assert.equal(fs.statSync(secretDir).mode & 0o777, 0o700);
  assert.equal(fs.statSync(path.join(secretDir, 'openai-api-key')).mode & 0o777, 0o600);
  assert.equal(fs.statSync(path.join(secretDir, 'voice-settings.json')).mode & 0o777, 0o600);
  assert.equal(loadConfig(env).transcribeModel, voice.model);
  assert.equal(loadConfig(env).openaiKey, voice.apiKey);
  assert.equal((await fetch(settingsUrl, options({ ...voice, apiKey: '' }))).status, 200);
  assert.equal(config.openaiKey, voice.apiKey);
  assert.equal((await fetch(settingsUrl, options({ ...voice, url: 'file:///etc/passwd' }))).status, 400);
  assert.equal(config.transcribeUrl, voice.url);
  assert.equal((await fetch(settingsUrl, options({ ...voice, apiKey: '', clearKey: true }))).status, 200);
  state = await (await fetch(`${base}/api/state`)).json();
  assert.equal(state.voice.available, false);
  assert.equal(state.voice.keySource, 'none');
  assert.equal(fs.existsSync(path.join(secretDir, 'openai-api-key')), false);
});
