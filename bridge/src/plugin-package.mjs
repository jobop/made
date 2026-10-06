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
  const allowed = new Set(['id', 'settings']);
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

export function resolveSettings(fields, raw) {
  if (raw === undefined || raw === null) raw = {};
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) throw fail('插件配置无效');
  const known = new Set(fields.map((field) => field.key));
  for (const key of Object.keys(raw)) {
    if (!known.has(key) || typeof raw[key] !== 'string' || Buffer.byteLength(raw[key]) > MAX_VALUE) throw fail('插件配置无效');
  }
  const values = {};
  for (const field of fields) {
    const value = Object.hasOwn(raw, field.key) ? raw[field.key] : (field.default ?? '');
    if (field.required && !value.trim()) throw fail('插件配置无效');
    if (field.pattern && !new RegExp(field.pattern).test(value)) throw fail('插件配置无效');
    values[field.key] = value;
  }
  return values;
}

export function readPackageSettings(dir) {
  const specFile = path.join(dir, 'plugin.json');
  if (!fs.existsSync(specFile)) return { id: '', fields: [], values: {}, error: '' };
  try {
    const spec = parseSettingsSpec(readJson(specFile));
    const settingsFile = path.join(dir, 'settings.json');
    const raw = fs.existsSync(settingsFile) ? readJson(settingsFile) : {};
    return { id: spec.id, fields: spec.fields, values: resolveSettings(spec.fields, raw), error: '' };
  } catch (error) {
    return { id: '', fields: [], values: {}, error: error.preserveMessage ? error.message : '插件配置无效' };
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

export function writePackageSettings(dir, values) {
  const current = readPackageSettings(dir);
  if (current.error) throw fail(current.error);
  if (!current.fields.length) throw fail('插件没有可保存的配置');
  const resolved = resolveSettings(current.fields, values);
  const file = path.join(dir, 'settings.json');
  const temporary = path.join(dir, `.settings-${process.pid}-${randomUUID()}.json`);
  fs.writeFileSync(temporary, `${JSON.stringify(resolved, null, 2)}\n`, { mode: 0o600 });
  fs.renameSync(temporary, file);
  return resolved;
}
