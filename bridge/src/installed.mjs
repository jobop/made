import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { pathToFileURL } from 'node:url';
import { loadConfiguredPlugins, validatePlugin } from './plugins/registry.mjs';
import { isPackageEntry, packageDirectories, readPackageSettings, writePackageSettings } from './plugin-package.mjs';

const PLUGIN_ID = /^[a-z0-9][a-z0-9_-]{0,39}$/;
const THEME_NAME = /^[A-Za-z0-9_-]{1,64}$/;
const MODULE_NAME = /^[A-Za-z0-9_-]+\.(?:mjs|js)$/;
const fail = (message) => {
  const error = new Error(message);
  error.preserveMessage = true;
  return error;
};

function pluginsDir(config) {
  return path.join(config.configDirectory, 'plugins');
}

function themesDir(config) {
  return path.join(config.configDirectory, 'themes');
}

function contained(root, target) {
  if (!fs.existsSync(root) || !fs.existsSync(target)) return false;
  const base = fs.realpathSync(root);
  const file = fs.realpathSync(target);
  const relative = path.relative(base, file);
  return relative !== '' && !relative.startsWith('..') && !path.isAbsolute(relative);
}

function moduleFiles(config) {
  const directory = pluginsDir(config);
  if (!fs.existsSync(directory) || !fs.statSync(directory).isDirectory()) return [];
  const packages = packageDirectories(directory).map((item) => item.entry).filter((filename) => contained(directory, filename));
  const loose = fs.readdirSync(directory).sort().flatMap((name) => {
    if (!MODULE_NAME.test(name)) return [];
    const filename = path.join(directory, name);
    if (!fs.statSync(filename).isFile() || !contained(directory, filename)) return [];
    return [filename];
  });
  return [...packages, ...loose];
}

function displayName(filename) {
  return isPackageEntry(filename) ? path.basename(path.dirname(filename)) : path.basename(filename);
}

function packageInfo(filename, pluginId) {
  if (!isPackageEntry(filename)) return { settingsSpec: [], settings: {}, settingsError: '' };
  const pack = readPackageSettings(path.dirname(filename));
  const mismatch = pack.id && pluginId && pack.id !== pluginId;
  const error = mismatch ? '插件配置规范与插件 ID 不一致' : pack.error;
  return {
    settingsSpec: error ? [] : pack.fields,
    settings: error ? {} : pack.values,
    settingsError: error,
  };
}

async function describeModule(filename, config) {
    const file = displayName(filename);
    try {
    const exported = (await import(pathToFileURL(filename).href)).default;
    const bundle = Array.isArray(exported) ? exported : [exported];
      return bundle.map((plugin) => {
      validatePlugin(plugin);
      if (plugin.kind !== 'coding-agent' && plugin.kind !== 'board-plugin') {
        throw new Error('外部插件仅支持助手或管控');
      }
      const pack = packageInfo(filename, plugin.id);
      let available = false;
      try {
        const result = plugin.probe(config);
        available = Boolean(result && typeof result === 'object' && !(result instanceof Promise) && result.available === true);
      } catch { /* Availability is display-only. */ }
      return {
        id: plugin.id,
        label: plugin.label,
        kind: plugin.kind,
        file,
        available,
        enabled: !(config.disabledPlugins || []).includes(plugin.id),
        events: plugin.kind === 'board-plugin' ? [...plugin.events] : [],
        commands: plugin.kind === 'board-plugin' ? [...plugin.commands] : [],
        ...pack,
      };
    });
  } catch {
    return [{ file, id: '', label: file, kind: '', available: false, enabled: false, events: [], commands: [], settingsSpec: [], settings: {}, settingsError: '', error: '插件无法加载' }];
  }
}

export async function listInstalled(config) {
  const plugins = [];
  for (const filename of moduleFiles(config)) plugins.push(...await describeModule(filename, config));
  return { plugins, themes: listThemes(config) };
}

export function listThemes(config) {
  const directory = themesDir(config);
  if (!fs.existsSync(directory) || !fs.statSync(directory).isDirectory()) return [];
  const themes = [];
  for (const name of fs.readdirSync(directory).sort()) {
    if (!THEME_NAME.test(name)) continue;
    const dir = path.join(directory, name);
    const manifestPath = path.join(dir, 'theme.json');
    if (!fs.statSync(dir).isDirectory() || !contained(directory, dir) || !fs.existsSync(manifestPath)) continue;
    let title = name;
    try {
      const raw = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
      if (typeof raw.title === 'string' && raw.title.trim()) title = raw.title.trim();
    } catch { /* A broken manifest still shows the folder name. */ }
    const files = fs.readdirSync(dir).filter((item) => {
      const file = path.join(dir, item);
      return /^[A-Za-z0-9_.-]+$/.test(item) && fs.statSync(file).isFile();
    });
    themes.push({
      name,
      title,
      sound: files.includes('chime.wav'),
      icon: files.includes('icon.bin'),
      background: files.includes('bg.bin'),
    });
  }
  return themes;
}

function writeConfig(config, mutate) {
  const file = config.configFile;
  if (!file || path.basename(file) === 'config.example.json') throw fail('请先复制 config.example.json 为 config.local.json');
  const raw = JSON.parse(fs.readFileSync(file, 'utf8'));
  mutate(raw);
  const temporary = path.join(path.dirname(file), `.config-${process.pid}-${randomUUID()}.json`);
  fs.writeFileSync(temporary, `${JSON.stringify(raw, null, 2)}\n`, { mode: 0o600 });
  fs.renameSync(temporary, file);
}

async function reload(config) {
  config.pluginsRuntime = await loadConfiguredPlugins(config);
}

function covers(targetReal, candidateReal) {
  if (candidateReal === targetReal) return true;
  const relative = path.relative(targetReal, candidateReal);
  return relative !== '' && !relative.startsWith('..') && !path.isAbsolute(relative);
}

function pluginPathInConfig(config, filename) {
  const target = fs.realpathSync(filename);
  return (config.plugins || []).filter((entry) => {
    if (typeof entry !== 'string' || !entry || /^[a-z]+:/i.test(entry)) return true;
    const resolved = path.resolve(config.configDirectory, entry);
    return !fs.existsSync(resolved) || !covers(target, fs.realpathSync(resolved));
  });
}

function removalTarget(config, filename) {
  const root = pluginsDir(config);
  const dir = path.dirname(filename);
  const base = path.basename(filename);
  if ((base === 'plugin.mjs' || base === 'plugin.js') && contained(root, dir) && path.dirname(fs.realpathSync(dir)) === fs.realpathSync(root)) return dir;
  return filename;
}

export async function setPluginEnabled(config, id, enabled) {
  if (!PLUGIN_ID.test(id || '')) throw fail('插件不存在');
  const installed = (await listInstalled(config)).plugins;
  if (!installed.some((plugin) => plugin.id === id)) throw fail('插件不存在');
  const previous = [...(config.disabledPlugins || [])];
  const next = new Set(previous);
  if (enabled) next.delete(id);
  else next.add(id);
  config.disabledPlugins = [...next];
  try {
    await reload(config);
  } catch (error) {
    config.disabledPlugins = previous;
    throw error;
  }
  try {
    writeConfig(config, (raw) => { raw.disabledPlugins = config.disabledPlugins; });
  } catch (error) {
    config.disabledPlugins = previous;
    await reload(config);
    throw error;
  }
  return listInstalled(config);
}

export async function savePluginSettings(config, id, values) {
  if (!PLUGIN_ID.test(id || '') || !values || typeof values !== 'object' || Array.isArray(values)) throw fail('插件配置无效');
  const filename = await entryForId(config, id);
  if (!filename || !isPackageEntry(filename)) throw fail('插件没有可保存的配置');
  const dir = path.dirname(filename);
  const resolved = writePackageSettings(dir, values);
  config.pluginSettings = { ...(config.pluginSettings || {}), [id]: resolved };
  if (config.pluginPackages?.[id]) config.pluginPackages[id] = { ...config.pluginPackages[id], values: resolved, error: '' };
  return listInstalled(config);
}

async function entryForId(config, id) {
  for (const candidate of moduleFiles(config)) {
    const described = await describeModule(candidate, config);
    if (described.some((plugin) => plugin.id === id)) return candidate;
  }
  return '';
}

export async function removePlugin(config, { id = '', file = '' } = {}) {
  const filename = moduleFiles(config).find((candidate) => {
    if (file) return displayName(candidate) === file;
    return false;
  });
  let target = filename;
  if (!target && id) {
    for (const candidate of moduleFiles(config)) {
      const described = await describeModule(candidate, config);
      if (described.some((plugin) => plugin.id === id)) target = candidate;
    }
  }
  if (!target || !contained(pluginsDir(config), target)) throw fail('插件不存在');
  target = removalTarget(config, target);
  const described = await describeModule(target, config);
  const ids = described.map((plugin) => plugin.id).filter((item) => PLUGIN_ID.test(item));
  const previousDisabled = [...(config.disabledPlugins || [])];
  const previousPlugins = [...(config.plugins || [])];
  config.disabledPlugins = [...new Set([...previousDisabled, ...ids])];
  config.plugins = pluginPathInConfig(config, target);
  try {
    await reload(config);
    writeConfig(config, (raw) => {
      raw.disabledPlugins = config.disabledPlugins;
      if (Array.isArray(raw.plugins)) raw.plugins = config.plugins;
    });
  } catch (error) {
    config.disabledPlugins = previousDisabled;
    config.plugins = previousPlugins;
    await reload(config).catch(() => {});
    throw error;
  }
  fs.rmSync(target, { recursive: true, force: true });
  config.disabledPlugins = previousDisabled.filter((item) => !ids.includes(item));
  try {
    await reload(config);
    writeConfig(config, (raw) => { raw.disabledPlugins = config.disabledPlugins; });
  } catch {
    // The file is already gone. Leaving it disabled still lets the bridge start.
  }
  return listInstalled(config);
}

export function removeTheme(config, name) {
  if (!THEME_NAME.test(name || '')) throw fail('主题不存在');
  const directory = themesDir(config);
  const target = path.join(directory, name);
  if (!fs.existsSync(target) || !fs.statSync(target).isDirectory() || !contained(directory, target)) throw fail('主题不存在');
  if (!fs.existsSync(path.join(target, 'theme.json'))) throw fail('主题不存在');
  fs.rmSync(target, { recursive: true, force: true });
  return listThemes(config);
}

export function themeExists(config, name) {
  return listThemes(config).some((theme) => theme.name === name);
}
