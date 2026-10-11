import fs from 'node:fs';
import os from 'node:os';
import { loadLanguageSettings } from './language-settings.mjs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadOpenAIKey } from './openai-key.mjs';
import { pluginRuntime } from './plugin-runtime.mjs';
import { DEFAULT_CODEX_MODEL, loadModelSettings, validateModelSettings, validateStoredModels } from './model-settings.mjs';
import { DEFAULT_TRANSCRIBE_MODEL, DEFAULT_TRANSCRIBE_URL, loadVoiceSettings, validateTranscribeModel, validateTranscribeUrl } from './voice-settings.mjs';

export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const PLUGIN_ID = /^[a-z0-9][a-z0-9_-]{0,39}$/;

/** 随代码发布的内置包目录：bridge/builtin/{plugins,themes}。升级代码即升级内置包。 */
export const BUILTIN_ROOT = path.join(ROOT, 'builtin');

/**
 * 用户包目录根：默认 ~/made，可用 MADE_HOME 环境变量或配置里的 madeHome 覆盖。
 *
 * 这是一个固定位置，不跟着配置文件走：市场把包装到 <root>/plugins 与 <root>/themes，
 * 桥接器每次启动自动扫描，换一个工作目录也不需要重新安装。
 */
export function packagesHome(config = {}) {
  return path.resolve(config.madeHome || process.env.MADE_HOME || path.join(os.homedir(), 'made'));
}

/**
 * 包目录的查找顺序：内置在前、用户目录在后，同名时用户目录覆盖内置。
 *
 * 内置目录只读，移除与改配置只作用于用户目录。
 */
export function packageRoots(config = {}) {
  return [BUILTIN_ROOT, packagesHome(config)];
}

/**
 * 校验 VIBE_USB_PORT。允许两类写法：绝对路径（/dev/cu.usbmodem1101、
 * \\.\COM10），以及 Windows 上用户真正会写的 COM 端口名（COM3）。
 *
 * 为什么要特判 COM3：Windows 上「COM3」既没有盘符也没有 UNC 根，
 * path.isAbsolute 判 false，只用绝对路径规则会把 Windows 唯一的手动兜底
 * 直接拦掉，而那个兜底正是 USB 在这套系统上唯一能走的路。
 */
export function validateUsbPort(value, platform = process.platform) {
  const port = String(value || '');
  if (!port) return '';
  if (/[\r\n\x00]/.test(port)) throw new Error('VIBE_USB_PORT 不能包含控制字符');
  if (path.isAbsolute(port)) return port;
  if (platform === 'win32' && /^\\{0,2}(?:\.\\)?COM\d+$/i.test(port)) return port;
  throw new Error('VIBE_USB_PORT 必须是 USB 串口的绝对路径，或 Windows 上的 COM 端口名');
}

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
  // 同时执行的会话数。同一会话恒为串行（编程工具按会话线程加写锁，并行必然冲突），
  // 所以这里限制的是"能同时跑几个会话"，不是"一个会话里能同时跑几条指令"。
  const maxParallelJobs = Number(env.VIBE_MAX_PARALLEL || raw.maxParallelJobs || 2);
  if (!Number.isInteger(maxParallelJobs) || maxParallelJobs < 1 || maxParallelJobs > 4) {
    throw new Error('并行任务数必须是 1 到 4 之间的整数');
  }
  const stateDir = path.resolve(env.VIBE_STATE_DIR || path.join(ROOT, 'state'));
  const usbPort = validateUsbPort(env.VIBE_USB_PORT || '');
  const models = { ...validateModelSettings({
    codex: env.VIBE_CODEX_MODEL || raw.codexModel || DEFAULT_CODEX_MODEL,
    cursor: env.VIBE_CURSOR_MODEL || raw.cursorModel || '',
    qoder: env.VIBE_QODER_MODEL || raw.qoderModel || '',
  }), ...validateStoredModels(raw.models || {}), ...(loadModelSettings(stateDir) || {}) };
  const asrMode = env.VIBE_ASR_MODE || 'openai';
  if (!PLUGIN_ID.test(asrMode)) throw new Error('VIBE_ASR_MODE 必须是语音插件 ID');
  if (raw.plugins !== undefined && (!Array.isArray(raw.plugins) || raw.plugins.some(p => typeof p !== 'string'))) throw new Error('plugins 必须是本地模块路径数组');
  if (raw.madeHome !== undefined && (typeof raw.madeHome !== 'string' || !raw.madeHome.trim())) throw new Error('madeHome 必须是非空字符串');
  if (raw.disabledPlugins !== undefined && (!Array.isArray(raw.disabledPlugins) || raw.disabledPlugins.some(p => !PLUGIN_ID.test(p)))) throw new Error('disabledPlugins 必须是插件 ID 数组');
  if (raw.pluginSettings !== undefined && (!raw.pluginSettings || typeof raw.pluginSettings !== 'object' || Array.isArray(raw.pluginSettings))) throw new Error('pluginSettings 必须是对象');
  const savedVoiceSettings = loadVoiceSettings(stateDir);
  const environmentOpenaiKey = env.OPENAI_API_KEY || '';
  const savedOpenaiKey = loadOpenAIKey(stateDir);
  return {
    configDirectory: base,
    madeHome: packagesHome({ madeHome: env.MADE_HOME || raw.madeHome }),
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
    maxParallelJobs,
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
    configFile: selected,
    // 插件可用的跨平台助手（命令查找、子进程、npm）。见 plugin-runtime.mjs。
    runtime: pluginRuntime,
  };
}
