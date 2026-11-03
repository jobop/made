import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';

const PACKAGE_DIR = /^[a-z0-9][a-z0-9_-]{0,39}$/;
const SETTING_KEY = /^[A-Za-z][A-Za-z0-9_]{0,31}$/;
const reserved = new Set(['__proto__', 'constructor', 'prototype']);
const MAX_FIELDS = 16;
const MAX_LABEL = 64;
const MAX_HELP = 200;
const MAX_VALUE = 500;
const MAX_PATTERN = 200;
const MAX_FILE = 16 * 1024;

const fail = (message) => {
  const error = new Error(message);
  error.preserveMessage = true;
  return error;
};

const textOK = (value, max) => typeof value === 'string' && value.trim() && Buffer.byteLength(value) <= max && !/[\p{Cc}\p{Cf}]/u.test(value);

function readJson(file) {
  const text = fs.readFileSync(file, 'utf8');
  if (Buffer.byteLength(text) > MAX_FILE) throw fail('插件配置过大');
  try {
    return JSON.parse(text);
  } catch {
    throw fail('插件配置无效');
  }
}

export function parseSettingsSpec(raw) {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) throw fail('插件配置规范无效');
  // h5 包的 plugin.json 额外携带 kind/www/capabilities（能力清单见 h5-apps.mjs），
  // 对设置解析是透明字段；助手包不带这些键，带上也不影响解析。
  const allowed = new Set(['id', 'settings', 'kind', 'www', 'capabilities']);
  if (Object.keys(raw).some((key) => !allowed.has(key))) throw fail('插件配置规范无效');
  if (raw.id !== undefined && (typeof raw.id !== 'string' || !PACKAGE_DIR.test(raw.id))) throw fail('插件配置规范无效');
  if (raw.settings === undefined) return { id: raw.id || '', fields: [] };
  if (!Array.isArray(raw.settings) || raw.settings.length > MAX_FIELDS) throw fail('插件配置规范无效');
  const keys = new Set();
  const fields = raw.settings.map((field) => {
    if (!field || typeof field !== 'object' || Array.isArray(field)) throw fail('插件配置规范无效');
    const fieldKeys = new Set(['key', 'label', 'type', 'required', 'default', 'help', 'pattern']);
    if (Object.keys(field).some((key) => !fieldKeys.has(key))) throw fail('插件配置规范无效');
    if (!SETTING_KEY.test(field.key || '') || reserved.has(field.key) || keys.has(field.key)) throw fail('插件配置规范无效');
    if (!textOK(field.label, MAX_LABEL) || field.type !== 'text') throw fail('插件配置规范无效');
    if (field.required !== undefined && typeof field.required !== 'boolean') throw fail('插件配置规范无效');
    if (field.default !== undefined && (typeof field.default !== 'string' || Buffer.byteLength(field.default) > MAX_VALUE)) throw fail('插件配置规范无效');
    if (field.help !== undefined && !textOK(field.help, MAX_HELP)) throw fail('插件配置规范无效');
    let pattern;
    if (field.pattern !== undefined) {
      if (typeof field.pattern !== 'string' || field.pattern.length > MAX_PATTERN) throw fail('插件配置规范无效');
      try {
        const compiled = new RegExp(field.pattern);
        if (compiled.global || compiled.sticky) throw fail('插件配置规范无效');
      } catch (error) {
        if (error.preserveMessage) throw error;
        throw fail('插件配置规范无效');
      }
      pattern = field.pattern;
    }
    keys.add(field.key);
    return {
      key: field.key,
      label: field.label.trim(),
      type: 'text',
      required: field.required === true,
      ...(field.default !== undefined ? { default: field.default } : {}),
      ...(field.help ? { help: field.help.trim() } : {}),
      ...(pattern ? { pattern } : {}),
    };
  });
  return { id: raw.id || '', fields };
}

// 云端资源环境变量白名单：插件配置里只有这些变量能被 ${...} 占位符引用。
// 语义契约：cloud 把自己的模型资源注入沙箱环境变量（MADE_LLM_BASE_URL /
// MADE_LLM_TOKEN / MADE_LLM_MODEL），开发者在自己 plugin.json 的字段 default
// 里显式引用（如 "${MADE_LLM_TOKEN}"）——字段语义只有开发者知道，cloud/桥接器
// 不做任何猜测。白名单同时防止把沙箱里其他环境变量泄漏进用户可见的配置视图。
const CLOUD_ENV_KEYS = new Set(['MADE_LLM_BASE_URL', 'MADE_LLM_TOKEN', 'MADE_LLM_MODEL']);
const CLOUD_ENV_PATTERN = /\$\{(MADE_LLM_[A-Z_]+)\}/g;

/** 解析配置值里的 ${MADE_LLM_*} 占位符；云端环境缺失（本地跑）时保留原样。 */
export function resolveCloudPlaceholders(value, env = process.env) {
  return String(value ?? '').replace(CLOUD_ENV_PATTERN, (whole, name) => {
    if (!CLOUD_ENV_KEYS.has(name)) return whole;
    const resolved = String(env[name] ?? '').trim();
    return resolved || whole;
  });
}

/** 字段默认值是否引用了云端资源变量（展示层据此标「☁ 云端提供」）。 */
export function fieldUsesCloudEnv(field) {
  for (const key of CLOUD_ENV_KEYS) {
    if (String(field?.default || '').includes(`\${${key}}`)) return true;
  }
  return false;
}

export function resolveSettings(fields, raw, env = process.env) {
  if (raw === undefined || raw === null) raw = {};
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) throw fail('插件配置无效');
  // 表单字段以插件自己的 plugin.json 为准。settings.json 里开发者另写的键
  // 不是桥接器的预定义名单，只要键名和值合法就原样收下，不能因此整表失效。
  const known = new Set(fields.map((field) => field.key));
  const values = {};
  for (const key of Object.keys(raw)) {
    if (reserved.has(key) || !SETTING_KEY.test(key)) throw fail('插件配置无效');
    if (typeof raw[key] !== 'string' || Buffer.byteLength(raw[key]) > MAX_VALUE) throw fail('插件配置无效');
    if (!known.has(key)) values[key] = raw[key];
  }
  for (const field of fields) {
    // 空串=未填：回落插件默认值，再做云端占位符解析，然后才校验格式。
    // （这样 default 里写 "${MADE_LLM_BASE_URL}" 这类占位符也能过 URL 类 pattern）
    const submitted = Object.hasOwn(raw, field.key) ? String(raw[field.key] ?? '') : '';
    const value = resolveCloudPlaceholders(submitted.trim() ? submitted : (field.default ?? ''), env);
    if (field.required && !value.trim()) throw fail('插件配置无效');
    if (field.pattern && !new RegExp(field.pattern).test(value)) throw fail('插件配置无效');
    values[field.key] = value;
  }
  return values;
}

export function readPackageSettings(dir, env = process.env) {
  const specFile = path.join(dir, 'plugin.json');
  if (!fs.existsSync(specFile)) return { id: '', fields: [], values: {}, saved: {}, error: '' };
  try {
    const spec = parseSettingsSpec(readJson(specFile));
    const settingsFile = path.join(dir, 'settings.json');
    const raw = fs.existsSync(settingsFile) ? readJson(settingsFile) : {};
    return {
      id: spec.id,
      fields: spec.fields,
      values: resolveSettings(spec.fields, raw, env),
      saved: raw,
      error: '',
    };
  } catch (error) {
    return { id: '', fields: [], values: {}, saved: {}, error: error.preserveMessage ? error.message : '插件配置无效' };
  }
}

export function isPackageEntry(filename) {
  const base = path.basename(filename);
  return (base === 'plugin.mjs' || base === 'plugin.js') && fs.existsSync(path.join(path.dirname(filename), 'plugin.json'));
}

export function packageDirectories(pluginsDir) {
  if (!fs.existsSync(pluginsDir) || !fs.statSync(pluginsDir).isDirectory()) return [];
  return fs.readdirSync(pluginsDir).sort().flatMap((name) => {
    if (!PACKAGE_DIR.test(name)) return [];
    const dir = path.join(pluginsDir, name);
    if (!fs.statSync(dir).isDirectory()) return [];
    const entry = ['plugin.mjs', 'plugin.js'].map((file) => path.join(dir, file)).find((file) => fs.existsSync(file) && fs.statSync(file).isFile());
    return entry ? [{ name, dir, entry }] : [];
  });
}

export function writePackageSettings(dir, values, env = process.env) {
  const current = readPackageSettings(dir, env);
  if (current.error) throw fail(current.error);
  if (!current.fields.length) throw fail('插件没有可保存的配置');
  // 工作台只提交 plugin.json 里的字段。写回时保留开发者另外放在 settings.json 的键
  // （kept 来自已解析视图，占位符会被固化；这些是开发者自留键，可接受）。
  const specKeys = new Set(current.fields.map((field) => field.key));
  const kept = {};
  for (const [key, value] of Object.entries(current.values)) {
    if (!specKeys.has(key)) kept[key] = value;
  }
  // 落盘写用户显式提交的值（空串保留空串=未填），不落 default 回落值与
  // 占位符解析值——否则云侧换令牌/模型后旧值固化，读盘回落机制失效。
  const merged = { ...kept, ...values };
  // 校验 + 运行时视图（空串回落 default + 占位符解析 + 格式校验）
  const resolved = resolveSettings(current.fields, merged, env);
  const disk = {};
  for (const [key, value] of Object.entries(merged)) {
    if (typeof value !== 'string' || Buffer.byteLength(value) > MAX_VALUE) throw fail('插件配置无效');
    disk[key] = value;
  }
  const file = path.join(dir, 'settings.json');
  const temporary = path.join(dir, `.settings-${process.pid}-${randomUUID()}.json`);
  fs.writeFileSync(temporary, `${JSON.stringify(disk, null, 2)}\n`, { mode: 0o600 });
  fs.renameSync(temporary, file);
  return resolved;
}
