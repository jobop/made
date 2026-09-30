import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { transcribe, transcriptionState } from '../src/transcribe.mjs';
import { initializeSpeechSettings, saveSpeechSettings } from '../src/speech-settings.mjs';
import { builtinSpeechRecognizers } from '../src/speech/index.mjs';
import { saveOpenAIKey, loadOpenAIKey } from '../src/openai-key.mjs';
import { saveVoiceSettings, loadVoiceSettings } from '../src/voice-settings.mjs';

function sampleWav() {
  const wav = Buffer.alloc(44 + 16000);
  wav.write('RIFF', 0); wav.writeUInt32LE(wav.length - 8, 4); wav.write('WAVEfmt ', 8);
  wav.writeUInt32LE(16, 16); wav.writeUInt16LE(1, 20); wav.writeUInt16LE(1, 22);
  wav.writeUInt32LE(16000, 24); wav.writeUInt32LE(32000, 28);
  wav.writeUInt16LE(2, 32); wav.writeUInt16LE(16, 34); wav.write('data', 36);
  wav.writeUInt32LE(16000, 40);
  return wav;
}

function fixture(t, extra = []) {
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-speech-plugins-'));
  t.after(() => fs.rmSync(stateDir, { recursive: true, force: true }));
  const plugins = [...builtinSpeechRecognizers, ...extra];
  const config = { stateDir, asrMode: 'openai', openaiKey: '', environmentOpenaiKey: '',
    pluginsRuntime: {
      speechRecognizers: () => plugins,
      speechRecognizer: (id) => plugins.find((plugin) => plugin.id === id),
    },
  };
  return config;
}

function custom(overrides = {}) {
  return { apiVersion: 1, kind: 'speech-recognizer', id: 'example-asr', label: 'Example ASR',
    fields: [
      { key: 'endpoint', label: '地址', type: 'url', required: true, default: 'https://example.test/transcribe' },
      { key: 'token', label: '密钥', type: 'secret', required: true },
      { key: 'model', label: '模型', type: 'model', default: 'custom' },
    ],
    probe: (settings) => ({ available: Boolean(settings.token) }),
    transcribe: async () => '测试语音任务', ...overrides,
  };
}

test('外部模块按协议转写；宿主统一校验 WAV 和返回内容', async (t) => {
  const config = fixture(t);
  const file = path.join(config.stateDir, 'example-asr.mjs');
  fs.writeFileSync(file, `export default {
    apiVersion: 1, kind: 'speech-recognizer', id: 'external-asr', label: 'External',
    fields: [{key:'model',label:'Model',type:'model',default:'external-model'}],
    probe: () => ({available:true}),
    async transcribe({wav,settings,signal}) {
      if (wav.length !== 16044 || settings.model !== 'external-model' || !signal) throw Error('bad contract');
      return '  来自插件的文字  ';
    }
  };`);
  const plugin = (await import(pathToFileURL(file))).default;
  config.pluginsRuntime.speechRecognizers = () => [...builtinSpeechRecognizers, plugin];
  config.pluginsRuntime.speechRecognizer = (id) => [...builtinSpeechRecognizers, plugin].find((p) => p.id === id);
  initializeSpeechSettings(config);
  saveSpeechSettings(config, { pluginId: plugin.id, settings: {} });
  assert.equal(await transcribe(sampleWav(), config), '来自插件的文字');
  assert.equal(transcriptionState(config).available, true);
  await assert.rejects(transcribe(Buffer.alloc(20), config), /录音大小/);
  for (const value of [null, { text: '错误类型' }, 123]) {
    plugin.transcribe = async () => value;
    await assert.rejects(transcribe(sampleWav(), config), /必须返回文字/);
  }
  plugin.transcribe = async () => '太长'.repeat(2000);
  await assert.rejects(transcribe(sampleWav(), config), /过长或过短/);
});

test('切换插件、重启和空白密码保留各自设置；清除只影响选中密钥', (t) => {
  const config = fixture(t, [custom()]);
  initializeSpeechSettings(config);
  saveSpeechSettings(config, { pluginId: 'openai', settings: { apiKey: 'saved-openai-secret', model: 'whisper-1' } });
  saveSpeechSettings(config, { pluginId: 'example-asr', settings: { token: 'separate-custom-secret', model: 'small' } });
  saveSpeechSettings(config, { pluginId: 'openai', settings: { apiKey: '' } });
  assert.equal(config.openaiKey, 'saved-openai-secret');
  saveSpeechSettings(config, { pluginId: 'example-asr', settings: {} });
  const restarted = { ...config, speech: undefined };
  initializeSpeechSettings(restarted);
  assert.equal(restarted.speech.pluginId, 'example-asr');
  assert.equal(restarted.speech.settingsByPlugin['example-asr'].token, 'separate-custom-secret');
  assert.equal(restarted.speech.settingsByPlugin.openai.apiKey, 'saved-openai-secret');
  let state = transcriptionState(restarted);
  assert.equal(state.secretConfigured.token, true);
  assert.equal(state.settings.model, 'small');
  assert.equal(JSON.stringify(state).includes('saved-openai-secret'), false);
  assert.equal(JSON.stringify(state).includes('separate-custom-secret'), false);
  assert.equal(Object.hasOwn(state.settings, 'token'), false);
  saveSpeechSettings(restarted, { pluginId: 'example-asr', settings: { token: '' }, clearSecrets: ['token'] });
  state = transcriptionState(restarted);
  assert.equal(state.available, false);
  assert.equal(state.secretConfigured.token, false);
  assert.equal(restarted.openaiKey, 'saved-openai-secret');
  assert.equal(fs.statSync(path.join(config.stateDir, 'secrets')).mode & 0o777, 0o700);
  assert.equal(fs.statSync(path.join(config.stateDir, 'secrets/speech-settings.json')).mode & 0o777, 0o600);
});

test('旧语音三要素和密钥无损迁移，环境密钥不被隐式保存', (t) => {
  const config = fixture(t);
  saveOpenAIKey(config.stateDir, 'legacy-api-key');
  saveVoiceSettings(config.stateDir, { url: 'https://api.siliconflow.cn/v1/audio/transcriptions', model: 'FunAudioLLM/SenseVoiceSmall' });
  const legacy = loadVoiceSettings(config.stateDir);
  Object.assign(config, { openaiKey: loadOpenAIKey(config.stateDir), openaiKeySource: 'saved',
    transcribeUrl: legacy.url, transcribeModel: legacy.model });
  initializeSpeechSettings(config);
  saveSpeechSettings(config, { pluginId: 'whisper', settings: {} });
  assert.equal(config.speech.settingsByPlugin.openai.apiKey, 'legacy-api-key');
  saveSpeechSettings(config, { pluginId: 'openai', settings: {} });
  assert.equal(config.transcribeUrl, legacy.url);
  assert.equal(config.transcribeModel, legacy.model);
  saveSpeechSettings(config, { pluginId: 'openai', settings: {}, clearSecrets: ['apiKey'] });
  assert.equal(loadOpenAIKey(config.stateDir), '');
  assert.equal(config.openaiKey, '');
  const environment = fixture(t);
  Object.assign(environment, { openaiKey: 'environment-key', environmentOpenaiKey: 'environment-key', openaiKeySource: 'environment' });
  initializeSpeechSettings(environment);
  saveSpeechSettings(environment, { pluginId: 'openai', settings: { model: 'whisper-1' } });
  assert.equal(environment.openaiKey, 'environment-key');
  assert.equal(environment.openaiKeySource, 'environment');
  assert.equal(fs.readFileSync(path.join(environment.stateDir, 'secrets/speech-settings.json'), 'utf8').includes('environment-key'), false);
});

test('错误字段和冲突清除请求不改变持久化配置', (t) => {
  const config = fixture(t, [custom()]);
  initializeSpeechSettings(config);
  saveSpeechSettings(config, { pluginId: 'example-asr', settings: { token: 'kept' } });
  const file = path.join(config.stateDir, 'secrets/speech-settings.json');
  const before = fs.readFileSync(file, 'utf8');
  for (const payload of [
    { pluginId: 'example-asr', settings: { unknown: 'bad' } },
    { pluginId: 'example-asr', settings: { token: 'other' }, clearSecrets: ['token'] },
    { pluginId: 'example-asr', settings: {}, clearSecrets: ['model'] },
    { pluginId: 'example-asr', settings: { endpoint: 'file:///etc/passwd' } },
    { pluginId: 'not-installed', settings: {} },
  ]) assert.throws(() => saveSpeechSettings(config, payload));
  assert.equal(fs.readFileSync(file, 'utf8'), before);
  assert.equal(config.speech.settingsByPlugin['example-asr'].token, 'kept');
});

test('取消立即结束等待，包括未响应 signal 的插件；取消前不调用插件', async (t) => {
  let called = 0;
  let begun;
  const started = new Promise((resolve) => { begun = resolve; });
  const plugin = custom({ transcribe: () => { called += 1; begun(); return new Promise(() => {}); } });
  const config = fixture(t, [plugin]);
  initializeSpeechSettings(config);
  saveSpeechSettings(config, { pluginId: plugin.id, settings: { token: 'cancellation-secret' } });
  const controller = new AbortController();
  const result = transcribe(sampleWav(), config, controller.signal);
  await started;
  controller.abort();
  await assert.rejects(result, { name: 'AbortError' });
  assert.equal(called, 1);
  await assert.rejects(transcribe(sampleWav(), config, controller.signal), { name: 'AbortError' });
  assert.equal(called, 1);
});

test('插件探测和执行报错时隐藏已声明密钥；卸载后保留配置但不执行', async (t) => {
  const plugin = custom({
    probe: (settings) => { throw new Error(`credential ${settings.token} failed`); },
    transcribe: async ({ settings }) => { throw new Error(`credential ${settings.token} failed`); },
  });
  const config = fixture(t, [plugin]);
  initializeSpeechSettings(config);
  saveSpeechSettings(config, { pluginId: plugin.id, settings: { token: 'sensitive-api-key' } });
  const state = transcriptionState(config);
  assert.equal(state.available, false);
  assert.equal(JSON.stringify(state).includes('sensitive-api-key'), false);
  await assert.rejects(transcribe(sampleWav(), config), (error) => !error.message.includes('sensitive-api-key'));
  config.pluginsRuntime.speechRecognizers = () => builtinSpeechRecognizers;
  config.pluginsRuntime.speechRecognizer = (id) => builtinSpeechRecognizers.find((p) => p.id === id);
  initializeSpeechSettings(config);
  assert.equal(config.speech.settingsByPlugin[plugin.id].token, 'sensitive-api-key');
  assert.equal(transcriptionState(config).available, false);
  assert.equal(transcriptionState(config).reason, '所选语音插件尚未安装');
  await assert.rejects(transcribe(sampleWav(), config), /尚未安装/);
});

test('异步 probe 的拒绝不会逃逸；validateSettings 报错和归一化不能泄密或绕过限制', async (t) => {
  const plugin = custom({ probe: async () => { throw new Error('async probe failure'); } });
  const config = fixture(t, [plugin]);
  initializeSpeechSettings(config);
  saveSpeechSettings(config, { pluginId: plugin.id, settings: { token: 'validator-secret' } });
  let state = transcriptionState(config);
  assert.equal(state.available, false);
  assert.match(state.reason, /必须同步/);
  await new Promise((resolve) => setImmediate(resolve));
  plugin.validateSettings = (settings) => { throw new Error(`bad secret: ${settings.token}`); };
  assert.throws(() => saveSpeechSettings(config, { pluginId: plugin.id, settings: {} }), (error) => {
    assert.equal(error.message.includes('validator-secret'), false);
    assert.match(error.message, /已隐藏密钥/);
    return true;
  });
  for (const override of [{ endpoint: 'file:///etc/passwd' }, { model: 'x'.repeat(257) }, { token: 'new\nline' }]) {
    plugin.validateSettings = (settings) => ({ ...settings, ...override });
    assert.throws(() => saveSpeechSettings(config, { pluginId: plugin.id, settings: {} }));
  }
  plugin.validateSettings = (settings) => ({ ...settings, token: '' });
  await assert.rejects(transcribe(sampleWav(), config), /请先填写密钥/);
});
