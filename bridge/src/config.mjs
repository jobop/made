import fs from 'node:fs';
import { loadLanguageSettings } from './language-settings.mjs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadOpenAIKey } from './openai-key.mjs';
import { DEFAULT_CODEX_MODEL, loadModelSettings, validateModelSettings, validateStoredModels } from './model-settings.mjs';
import { DEFAULT_TRANSCRIBE_MODEL, DEFAULT_TRANSCRIBE_URL, loadVoiceSettings, validateTranscribeModel, validateTranscribeUrl } from './voice-settings.mjs';

export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const PLUGIN_ID = /^[a-z0-9][a-z0-9_-]{0,39}$/;

export function loadConfig(env = process.env) {
  const configPath = path.resolve(env.VIBE_CONFIG || path.join(ROOT, 'config.local.json'));
  const fallback = path.join(ROOT, 'config.example.json');
  const selected = fs.existsSync(configPath) ? configPath : fallback;
  const raw = JSON.parse(fs.readFileSync(selected, 'utf8'));
  const base = path.dirname(selected);
  if (!Array.isArray(raw.projects) || raw.projects.length === 0) {
    throw new Error('配置需要至少一个 projects 项目');
  }
  const seen = new Set();
  const projects = raw.projects.map((project) => {
    if (!/^[a-z0-9_-]{1,40}$/.test(project.id) || seen.has(project.id)) {
      throw new Error(`项目 ID 无效或重复：${project.id}`);
    }
    seen.add(project.id);
    return {
      id: project.id,
      label: String(project.label || project.id),
      path: path.resolve(base, project.path),
    };
  });
  const defaultProject = raw.defaultProject || projects[0].id;
  const defaultProvider = raw.defaultProvider || 'codex';
  if (!seen.has(defaultProject) || !PLUGIN_ID.test(defaultProvider)) {
    throw new Error('默认项目或默认编程工具不在配置中');
  }
  const port = Number(raw.port || 8787);
  const devicePort = Number(raw.devicePort || 8788);
  if (![port, devicePort].every((p) => Number.isInteger(p) && p > 0 && p < 65536)) {
    throw new Error('端口号必须在 1 到 65535 之间');
  }
  const stateDir = path.resolve(env.VIBE_STATE_DIR || path.join(ROOT, 'state'));
  const usbPort = env.VIBE_USB_PORT || '';
  if (usbPort && (!path.isAbsolute(usbPort) || /[\r\n\x00]/.test(usbPort))) {
    throw new Error('VIBE_USB_PORT 必须是 USB 串口的绝对路径');
  }
  const models = { ...validateModelSettings({
    codex: env.VIBE_CODEX_MODEL || raw.codexModel || DEFAULT_CODEX_MODEL,
    cursor: env.VIBE_CURSOR_MODEL || raw.cursorModel || '',
    qoder: env.VIBE_QODER_MODEL || raw.qoderModel || '',
  }), ...validateStoredModels(raw.models || {}), ...(loadModelSettings(stateDir) || {}) };
  const asrMode = env.VIBE_ASR_MODE || 'openai';
  if (!PLUGIN_ID.test(asrMode)) throw new Error('VIBE_ASR_MODE 必须是语音插件 ID');
  if (raw.plugins !== undefined && (!Array.isArray(raw.plugins) || raw.plugins.some(p => typeof p !== 'string'))) throw new Error('plugins 必须是本地模块路径数组');
  if (raw.disabledPlugins !== undefined && (!Array.isArray(raw.disabledPlugins) || raw.disabledPlugins.some(p => !PLUGIN_ID.test(p)))) throw new Error('disabledPlugins 必须是插件 ID 数组');
  if (raw.pluginSettings !== undefined && (!raw.pluginSettings || typeof raw.pluginSettings !== 'object' || Array.isArray(raw.pluginSettings))) throw new Error('pluginSettings 必须是对象');
  const savedVoiceSettings = loadVoiceSettings(stateDir);
  const environmentOpenaiKey = env.OPENAI_API_KEY || '';
  const savedOpenaiKey = loadOpenAIKey(stateDir);
  return {
    configDirectory: base,
    locale: loadLanguageSettings(stateDir, raw.locale ?? 'zh-CN'),
    plugins: raw.plugins || [],
    disabledPlugins: raw.disabledPlugins || [],
    pluginSettings: raw.pluginSettings || {},
    projects,
    workspaceRoot: path.resolve(base, raw.workspaceRoot || './workspaces'),
    defaultProject,
    defaultProvider,
    port,
    devicePort,
    codexModel: models.codex,
    models,
    asrMode,
    whisperModel: env.VIBE_WHISPER_MODEL || '',
    whisperCli: env.VIBE_WHISPER_CLI || 'whisper-cli',
    openaiKey: savedOpenaiKey || environmentOpenaiKey,
    openaiKeySource: savedOpenaiKey ? 'saved' : environmentOpenaiKey ? 'environment' : 'none',
    environmentOpenaiKey,
    transcribeUrl: savedVoiceSettings?.url || validateTranscribeUrl(env.VIBE_TRANSCRIBE_URL || DEFAULT_TRANSCRIBE_URL),
    transcribeModel: savedVoiceSettings?.model || validateTranscribeModel(env.VIBE_TRANSCRIBE_MODEL || DEFAULT_TRANSCRIBE_MODEL),
    workbuddyToken: env.WORKBUDDY_ACCESS_TOKEN || '',
    workbuddy: {
      allowUnscopedDispatch: raw.workbuddy?.allowUnscopedDispatch === true,
    },
    stateDir,
    usbPort,
  };
}
