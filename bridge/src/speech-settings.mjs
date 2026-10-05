import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { builtinSpeechRecognizers } from './speech/index.mjs';
import { DEFAULT_TRANSCRIBE_MODEL, DEFAULT_TRANSCRIBE_URL, saveVoiceSettings } from './voice-settings.mjs';
import { saveOpenAIKey } from './openai-key.mjs';

const SETTINGS_VERSION = 1;
const own = (object, key) => Object.prototype.hasOwnProperty.call(object || {}, key);
const isRecord = (value) => value !== null && typeof value === 'object' && !Array.isArray(value);
const settingsPath = (stateDir) => path.join(stateDir, 'secrets', 'speech-settings.json');

export function speechRecognizers(config) {
  return config.pluginsRuntime?.speechRecognizers() || builtinSpeechRecognizers;
}

export function speechRecognizer(config, id) {
  return config.pluginsRuntime ? config.pluginsRuntime.speechRecognizer(id) : builtinSpeechRecognizers.find((plugin) => plugin.id === id);
}

function legacySettings(config) {
  return {
    openai: {
      url: config.transcribeUrl || DEFAULT_TRANSCRIBE_URL,
      model: config.transcribeModel || DEFAULT_TRANSCRIBE_MODEL,
      apiKey: config.openaiKeySource === 'environment' ? '' : (config.openaiKey || ''),
    },
    whisper: { command: config.whisperCli || 'whisper-cli', modelPath: config.whisperModel || '' },
    off: {},
  };
}

function defaultSettings(plugin) {
  return Object.fromEntries(plugin.fields.map((field) => [field.key, field.default ?? '']));
}

function validateFields(plugin, settings, required) {
  if (!isRecord(settings)) throw new Error('语音插件设置格式无效');
  const keys = new Set(plugin.fields.map((field) => field.key));
  for (const key of Object.keys(settings)) if (!keys.has(key)) throw new Error(`语音插件不支持设置项：${key}`);
  for (const field of plugin.fields) {
    const value = settings[field.key];
    if (typeof value !== 'string' || value.length > (field.type === 'model' ? 256 : 4096) || /[\x00-\x1f\x7f]/.test(value)) {
      throw new Error(`${field.label} 格式无效`);
    }
    if (required && field.required && !value.trim()) throw new Error(`请先填写${field.label}`);
    if (field.type === 'url' && value) {
      let parsed;
      try { parsed = new URL(value); } catch { throw new Error(`${field.label} URL 无效`); }
      if (value !== value.trim() || !['https:', 'http:'].includes(parsed.protocol) || parsed.username || parsed.password || parsed.hash) {
        throw new Error(`${field.label} 须使用不含账号、密码和片段的 HTTP 或 HTTPS 地址`);
      }
    }
  }
  return settings;
}

export function normalizeSpeechSettings(plugin, values, { required = false } = {}) {
  let settings;
  let normalized;
  try {
    if (!isRecord(values)) throw new Error('语音插件设置格式无效');
    settings = validateFields(plugin, { ...defaultSettings(plugin), ...values }, required);
    if (!plugin.validateSettings) return settings;
    normalized = plugin.validateSettings({ ...settings });
    // A plugin can normalize its fields, but cannot bypass host bounds or add fields.
    return validateFields(plugin, normalized, required);
  } catch (error) {
    const message = redactSpeechError(error, plugin, settings || values || {});
    throw new Error(redactSpeechError(new Error(message), plugin, normalized || {}));
  }
}

export function initializeSpeechSettings(config) {
  let saved;
  try {
    const file = settingsPath(config.stateDir);
    if (!fs.lstatSync(file).isFile()) throw new Error('语音插件设置文件无效');
    saved = JSON.parse(fs.readFileSync(file, 'utf8'));
    if (saved.version !== SETTINGS_VERSION || typeof saved.pluginId !== 'string' || !isRecord(saved.settingsByPlugin)) {
      throw new Error('语音插件设置文件格式无效');
    }
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
  }
  const settingsByPlugin = { ...legacySettings(config), ...(saved?.settingsByPlugin || {}) };
  // Keep settings of temporarily uninstalled plugins so reinstalling is lossless.
  for (const plugin of speechRecognizers(config)) {
    if (own(settingsByPlugin, plugin.id)) settingsByPlugin[plugin.id] = normalizeSpeechSettings(plugin, settingsByPlugin[plugin.id]);
  }
  config.speech = { pluginId: saved?.pluginId || config.asrMode || 'openai', settingsByPlugin };
  syncLegacyFields(config);
  return config.speech;
}

export function speechSelection(config, pluginId = config.speech?.pluginId || config.asrMode || 'openai') {
  const plugin = speechRecognizer(config, pluginId);
  if (!plugin) return { pluginId, plugin: null, settings: {} };
  const stored = config.speech?.settingsByPlugin?.[pluginId] || legacySettings(config)[pluginId] || {};
  const settings = { ...defaultSettings(plugin), ...stored };
  if (pluginId === 'openai' && !settings.apiKey) {
    settings.apiKey = config.environmentOpenaiKey || (!config.speech && config.openaiKey) || '';
  }
  return { pluginId, plugin, settings };
}

function syncLegacyFields(config) {
  const openai = speechSelection(config, 'openai').settings;
  const whisper = speechSelection(config, 'whisper').settings;
  config.asrMode = config.speech.pluginId;
  config.transcribeUrl = openai.url || DEFAULT_TRANSCRIBE_URL;
  config.transcribeModel = openai.model || DEFAULT_TRANSCRIBE_MODEL;
  config.openaiKey = openai.apiKey || '';
  config.openaiKeySource = config.speech.settingsByPlugin.openai?.apiKey ? 'saved' : config.openaiKey ? 'environment' : 'none';
  config.whisperCli = whisper.command || 'whisper-cli';
  config.whisperModel = whisper.modelPath || '';
}

export function saveSpeechSettings(config, payload) {
  if (!isRecord(payload) || typeof payload.pluginId !== 'string') throw new Error('请选择语音识别插件');
  const plugin = speechRecognizer(config, payload.pluginId);
  if (!plugin) throw new Error('未安装所选语音识别插件');
  if (!isRecord(payload.settings || {})) throw new Error('语音插件设置格式无效');
  const clearSecrets = payload.clearSecrets || [];
  if (!Array.isArray(clearSecrets) || clearSecrets.some((key) => !plugin.fields.some((field) => field.key === key && field.type === 'secret'))) {
    throw new Error('要清除的语音密钥字段无效');
  }
  const current = config.speech || { pluginId: config.asrMode || 'openai', settingsByPlugin: legacySettings(config) };
  const updated = { ...defaultSettings(plugin), ...(current.settingsByPlugin[payload.pluginId] || {}) };
  for (const [key, value] of Object.entries(payload.settings || {})) {
    const field = plugin.fields.find((entry) => entry.key === key);
    if (!field) throw new Error(`语音插件不支持设置项：${key}`);
    // Blank passwords are placeholders in the UI, not requests to erase them.
    if (field.type === 'secret' && value === '') continue;
    updated[key] = value;
  }
  for (const key of clearSecrets) {
    if (payload.settings?.[key]) throw new Error('不能同时设置和清除同一语音密钥');
    updated[key] = '';
  }
  const normalized = normalizeSpeechSettings(plugin, updated);
  const next = { pluginId: payload.pluginId, settingsByPlugin: { ...current.settingsByPlugin, [payload.pluginId]: normalized } };
  const directory = path.join(config.stateDir, 'secrets');
  fs.mkdirSync(directory, { recursive: true, mode: 0o700 });
  fs.chmodSync(directory, 0o700);
  const file = settingsPath(config.stateDir);
  const temporary = path.join(directory, `.speech-settings-${process.pid}-${randomUUID()}`);
  try {
    fs.writeFileSync(temporary, JSON.stringify({ version: SETTINGS_VERSION, ...next }), { flag: 'wx', mode: 0o600 });
    fs.renameSync(temporary, file);
    fs.chmodSync(file, 0o600);
  } finally {
    fs.rmSync(temporary, { force: true });
  }
  config.speech = next;
  syncLegacyFields(config);
  // Preserve compatibility with existing integrations and older bridge versions.
  if (payload.pluginId === 'openai') {
    saveVoiceSettings(config.stateDir, { url: normalized.url, model: normalized.model });
    saveOpenAIKey(config.stateDir, normalized.apiKey || '');
  }
  return speechSettingsState(config);
}

export function redactSpeechError(error, plugin, settings) {
  let message = error instanceof Error ? error.message : '语音识别插件执行失败';
  for (const field of plugin?.fields || []) {
    if (field.type === 'secret' && typeof settings[field.key] === 'string' && settings[field.key]) message = message.split(settings[field.key]).join('[已隐藏密钥]');
  }
  return message.slice(0, 500);
}

function pluginState(config, plugin) {
  const { settings } = speechSelection(config, plugin.id);
  const publicSettings = {};
  const secretConfigured = {};
  for (const field of plugin.fields) {
    if (field.type === 'secret') secretConfigured[field.key] = Boolean(settings[field.key]);
    else publicSettings[field.key] = settings[field.key];
  }
  let probe;
  try {
    normalizeSpeechSettings(plugin, settings, { required: true });
    probe = plugin.probe(settings, { config });
    if (probe && typeof probe.then === 'function') {
      Promise.resolve(probe).catch(() => {});
      throw new Error('语音插件 probe 必须同步返回 available');
    }
    if (!probe || typeof probe.available !== 'boolean') throw new Error('语音插件 probe 必须同步返回 available');
  } catch (error) {
    probe = { available: false, reason: redactSpeechError(error, plugin, settings) };
  }
  return {
    id: plugin.id, label: plugin.label,
    fields: plugin.fields.map(({ key, label, type, required, default: defaultValue, placeholder }) => ({ key, label, type, required, ...(type === 'secret' ? {} : { default: defaultValue }), placeholder })),
    presets: (plugin.presets || []).map(({ id, label, values }) => ({ id, label, values: Object.fromEntries(Object.entries(values).filter(([key]) => plugin.fields.some((field) => field.key === key && field.type !== 'secret'))) })),
    settings: publicSettings, secretConfigured,
    available: probe.available,
    reason: typeof probe.reason === 'string' ? redactSpeechError(new Error(probe.reason), plugin, settings) : '',
  };
}

export function speechSettingsState(config) {
  const pluginId = config.speech?.pluginId || config.asrMode || 'openai';
  const plugins = speechRecognizers(config).map((plugin) => pluginState(config, plugin));
  const selected = plugins.find((plugin) => plugin.id === pluginId);
  return { pluginId, plugins, settings: selected?.settings || {}, secretConfigured: selected?.secretConfigured || {},
    available: selected?.available || false, reason: selected?.reason || (selected ? '' : '所选语音插件尚未安装') };
}
