import { pathToFileURL } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';
import { MODEL_ID } from '../model-settings.mjs';
import { builtinAgents } from '../agents/index.mjs';
import { normalizeAgentIcon } from '../agent-icons.mjs';
import { builtinSpeechRecognizers } from '../speech/index.mjs';

export const PLUGIN_API_VERSION = 1;
export const MAX_DEVICE_AGENTS = 12;
export const PLUGIN_ID = /^[a-z0-9][a-z0-9_-]{0,39}$/;
const reserved = new Set(['__proto__', 'constructor', 'prototype']);
const isRecord = (v) => v && typeof v === 'object' && !Array.isArray(v);
const labelOK = (v) => typeof v === 'string' && v.trim() && Buffer.byteLength(v) <= 64 && !/[\p{Cc}\p{Cf}]/u.test(v);
export function validatePlugin(plugin) {
  const fail = (message) => { throw new Error(`插件 ${plugin?.id || '(未知)'}：${message}`); };
  if (!isRecord(plugin) || plugin.apiVersion !== PLUGIN_API_VERSION) fail('不支持的 apiVersion，当前为 1');
  if (!PLUGIN_ID.test(plugin.id) || reserved.has(plugin.id)) fail('ID 必须为 1–40 个小写字母、数字、下划线或连字符');
  if (!labelOK(plugin.label)) fail('名称必须为不超过 64 字节的可见文字');
  if (typeof plugin.probe !== 'function') fail('缺少同步 probe');
  if (plugin.kind === 'coding-agent') {
    const c = plugin.capabilities;
    if (!isRecord(c) || !['native', 'external_unscoped'].includes(c.session) ||
        ['model', 'cancel', 'progress'].some((k) => typeof c[k] !== 'boolean')) fail('capabilities 无效');
    if (typeof plugin.run !== 'function') fail('缺少 run');
    try { normalizeAgentIcon(plugin.icon); } catch (error) { fail(error.message); }
    if (plugin.sessionIdPattern !== undefined && (!(plugin.sessionIdPattern instanceof RegExp) || plugin.sessionIdPattern.global || plugin.sessionIdPattern.sticky)) fail('sessionIdPattern 必须是不含 g/y 标志的正则表达式');
    if (plugin.model && (!c.model || !isRecord(plugin.model) || typeof plugin.model.default !== 'string' || typeof plugin.model.required !== 'boolean')) fail('model 元数据无效');
    if (plugin.model && ((!plugin.model.default && plugin.model.required) || (plugin.model.default && !MODEL_ID.test(plugin.model.default)))) fail('model 默认值无效');
    for (const k of ['readUpdates', 'syncJobState']) if (plugin[k] !== undefined && typeof plugin[k] !== 'function') fail(`${k} 必须是函数`);
  } else if (plugin.kind === 'board-plugin') {
    const names = (key, min) => {
      const list = plugin[key];
      if (!Array.isArray(list) || list.length > 16 || list.length < min || list.some((name) => typeof name !== 'string' || !/^[a-z][a-z0-9_]{0,15}(?:\.[a-z][a-z0-9_]{0,15}){0,2}$/.test(name))) fail(`${key} 无效`);
      if (new Set(list).size !== list.length) fail(`${key} 有重复项`);
    };
    names('events', 1);
    names('commands', 0);
    if (typeof plugin.onEvent !== 'function') fail('缺少 onEvent');
  } else if (plugin.kind === 'speech-recognizer') {
    if (typeof plugin.transcribe !== 'function' || !Array.isArray(plugin.fields) || plugin.fields.length > 16) fail('缺少 transcribe 或 fields 无效');
    const keys = new Set();
    for (const f of plugin.fields) {
      if (!isRecord(f) || !/^[A-Za-z][A-Za-z0-9_]{0,39}$/.test(f.key) || reserved.has(f.key) || keys.has(f.key) || !labelOK(f.label) || !['text', 'secret', 'url', 'model', 'path'].includes(f.type)) fail('配置字段无效或重复');
      if (f.default !== undefined && (typeof f.default !== 'string' || f.default.length > 4096 || (f.type === 'secret' && f.default))) fail('字段默认值无效；密钥不能有默认值');
      if (f.required !== undefined && typeof f.required !== 'boolean') fail('字段 required 必须是布尔值');
      keys.add(f.key);
    }
    if (plugin.validateSettings !== undefined && typeof plugin.validateSettings !== 'function') fail('validateSettings 必须是函数');
    if (plugin.presets !== undefined) {
      if (!Array.isArray(plugin.presets) || plugin.presets.length > 16) fail('presets 无效');
      const ids = new Set();
      for (const p of plugin.presets) {
        if (!isRecord(p) || !PLUGIN_ID.test(p.id) || ids.has(p.id) || !labelOK(p.label) || !isRecord(p.values)) fail('预设无效');
        for (const [key, value] of Object.entries(p.values)) if (!keys.has(key) || typeof value !== 'string' || plugin.fields.find(f => f.key === key).type === 'secret') fail('预设不能包含密钥或未声明字段');
        ids.add(p.id);
      }
    }
  } else fail('kind 应为 coding-agent、board-plugin 或 speech-recognizer');
  return plugin;
}

export function createPluginRegistry(plugins = [...builtinAgents, ...builtinSpeechRecognizers]) {
  const entries = new Map();
  for (const raw of plugins) {
    const plugin = validatePlugin(raw);
    if (entries.has(plugin.id)) throw new Error(`插件 ID 重复：${plugin.id}`);
    entries.set(plugin.id, plugin.kind === 'coding-agent' ? Object.freeze({ ...plugin, icon: normalizeAgentIcon(plugin.icon) }) : plugin);
  }
  const agents = [...entries.values()].filter(p => p.kind === 'coding-agent');
  const speech = [...entries.values()].filter(p => p.kind === 'speech-recognizer');
  const boards = [...entries.values()].filter(p => p.kind === 'board-plugin');
  if (agents.length > MAX_DEVICE_AGENTS) throw new Error(`码得最多支持 ${MAX_DEVICE_AGENTS} 个助手，请禁用多余插件`);
  return Object.freeze({
    agents: () => [...agents], agent: (id) => agents.find(p => p.id === id), hasAgent: (id) => agents.some(p => p.id === id),
    speechRecognizers: () => [...speech], speechRecognizer: (id) => speech.find(p => p.id === id), hasSpeechRecognizer: (id) => speech.some(p => p.id === id),
    boardPlugins: () => [...boards], boardPlugin: (id) => boards.find(p => p.id === id),
  });
}

export async function loadConfiguredPlugins(config) {
  const disabled = new Set(config.disabledPlugins || []);
  for (const recognizer of builtinSpeechRecognizers) {
    if (disabled.has(recognizer.id)) throw new Error(`语音识别 ${recognizer.id} 是内置接口，不能通过 disabledPlugins 禁用；请在语音设置中配置`);
  }
  const plugins = [...builtinAgents.filter(p => !disabled.has(p.id)), ...builtinSpeechRecognizers];
  const loadFile = async (filename, label) => {
    if (!/\.(mjs|js)$/.test(filename) || !fs.statSync(filename).isFile()) throw new Error(`插件模块无效：${label}`);
    const exported = (await import(pathToFileURL(filename).href)).default;
    const bundle = Array.isArray(exported) ? exported : [exported];
    for (const p of bundle) {
      validatePlugin(p);
      if (p.kind !== 'coding-agent' && p.kind !== 'board-plugin') throw new Error(`插件 ${p.id}：外部插件仅支持 coding-agent；语音识别请配置兼容接口的 URL、API Key 和模型名`);
      if (!disabled.has(p.id)) plugins.push(p);
    }
  };
  const explicitPaths = new Set();
  for (const entry of config.plugins || []) {
    if (typeof entry !== 'string' || !entry || /^[a-z]+:/i.test(entry)) throw new Error('plugins 仅接受显式的本地模块路径');
    const filename = path.resolve(config.configDirectory || process.cwd(), entry);
    if (!fs.existsSync(filename)) throw new Error(`插件模块无效：${entry}`);
    explicitPaths.add(fs.realpathSync(filename));
    await loadFile(filename, entry);
  }
  const pluginsDir = path.join(config.configDirectory || process.cwd(), 'plugins');
  if (fs.existsSync(pluginsDir) && fs.statSync(pluginsDir).isDirectory()) {
    for (const name of fs.readdirSync(pluginsDir).sort()) {
      if (!/\.(mjs|js)$/.test(name)) continue;
      const filename = path.join(pluginsDir, name);
      if (!fs.statSync(filename).isFile()) continue;
      if (explicitPaths.has(fs.realpathSync(filename))) continue;
      await loadFile(filename, `./plugins/${name}`);
    }
  }
  const registry = createPluginRegistry(plugins);
  if (!registry.hasAgent(config.defaultProvider)) throw new Error('默认编程助手未安装或已禁用');
  return registry;
}
