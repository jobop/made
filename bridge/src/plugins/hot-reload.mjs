import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { registerHooks } from 'node:module';
import { packageRoots } from '../config.mjs';
import { loadConfiguredPlugins } from './registry.mjs';

// 钩子和热加载在同一线程，闭包里的代数对 resolve 可见。
// 只改写插件目录内的相对导入，给它们带上同一代数，子模块才不会停在旧缓存里。
let generation = 0;
let roots = [];
let generationClock = 0;

function localPath(urlString) {
  try {
    const url = new URL(urlString);
    if (url.protocol !== 'file:') return '';
    url.search = '';
    url.hash = '';
    return fileURLToPath(url);
  } catch {
    return '';
  }
}

function underRoot(filePath) {
  if (!filePath) return false;
  return roots.some((root) => filePath.startsWith(root));
}

registerHooks({
  resolve(specifier, context, nextResolve) {
    if (!generation || !context.parentURL) return nextResolve(specifier, context);
    const parentPath = localPath(context.parentURL);
    if (!underRoot(parentPath)) return nextResolve(specifier, context);
    const resolved = nextResolve(specifier, context);
    const target = localPath(resolved.url);
    if (!target || target.includes(`${path.sep}node_modules${path.sep}`)) return resolved;
    if (!underRoot(target)) return resolved;
    const url = new URL(resolved.url);
    url.searchParams.set('hot', String(generation));
    return { ...resolved, url: url.href };
  },
});

function addDir(set, dir) {
  try {
    if (!dir || !fs.existsSync(dir) || !fs.statSync(dir).isDirectory()) return;
    set.add(fs.realpathSync(dir) + path.sep);
  } catch { /* 目录不可读就跳过，不影响已经加载的插件。 */ }
}

/** 监视范围：内置与用户 plugins/，以及配置里点名的本地模块所在目录。 */
export function pluginWatchRoots(config) {
  const found = new Set();
  for (const root of packageRoots(config)) addDir(found, path.join(root, 'plugins'));
  for (const entry of config.plugins || []) {
    if (typeof entry !== 'string' || !entry || /^[a-z]+:/i.test(entry)) continue;
    const filename = path.resolve(config.configDirectory || process.cwd(), entry);
    if (!fs.existsSync(filename)) continue;
    addDir(found, path.dirname(filename));
  }
  return [...found];
}

function publish(config) {
  generation = Number(config.pluginReloadGeneration) || 0;
  roots = pluginWatchRoots(config);
}

/**
 * 重新 import 已安装插件并换上注册表。失败时配置和注册表保持热加载前的那一份。
 * 正在执行的 run() 仍是旧函数；下一轮、下一次目录刷新才用新代码。
 */
export async function reloadConfiguredPlugins(config) {
  const snapshot = {
    pluginsRuntime: config.pluginsRuntime,
    pluginPackages: config.pluginPackages,
    pluginSettings: config.pluginSettings,
  };
  config.pluginReloadGeneration = ++generationClock;
  publish(config);
  try {
    config.pluginsRuntime = await loadConfiguredPlugins(config);
    return config.pluginsRuntime;
  } catch (error) {
    config.pluginsRuntime = snapshot.pluginsRuntime;
    config.pluginPackages = snapshot.pluginPackages;
    config.pluginSettings = snapshot.pluginSettings;
    throw error;
  }
}

const RELOAD_DELAY_MS = 300;

function relevantChange(filename) {
  if (!filename) return true;
  const normalized = filename.split(path.sep).join('/');
  if (normalized === 'node_modules' || normalized.startsWith('node_modules/') || normalized.includes('/node_modules/')) return false;
  return /\.(mjs|js|cjs|json|png)$/i.test(normalized);
}

/**
 * 监视插件文件。保存后约 300ms 热加载。
 * onReload 收到 null 表示成功，否则是保留了上一份的错误。
 */
export function watchPlugins(config, { onReload } = {}) {
  const watchers = [];
  const watched = [];
  let timer = null;
  let closed = false;
  let reloading = false;
  let pending = false;

  const arm = (target, recursive) => {
    let real;
    try { real = fs.realpathSync(target); } catch { return; }
    if (watched.some((item) => item.path === real && item.recursive === recursive)) return;
    watched.push({ path: real, recursive });
    try {
      watchers.push(fs.watch(target, { recursive }, (_event, filename) => {
        if (!relevantChange(filename)) return;
        schedule();
      }));
    } catch (error) {
      console.error(`[plugins] 无法监视 ${target}：${error.message}`);
    }
  };

  const covered = (dir) => {
    let real;
    try { real = fs.realpathSync(dir); } catch { return false; }
    return watched.some((item) => item.recursive && (real === item.path || real.startsWith(item.path + path.sep)));
  };

  async function run() {
    if (closed) return;
    if (reloading) { pending = true; return; }
    reloading = true;
    try {
      await reloadConfiguredPlugins(config);
      const ids = config.pluginsRuntime?.agents?.().map((plugin) => plugin.id) ?? [];
      console.error(`[plugins] 已热加载${ids.length ? `：${ids.join(', ')}` : ''}`);
      onReload?.(null);
    } catch (error) {
      console.error(`[plugins] 热加载失败，继续使用上一份：${error.message}`);
      onReload?.(error);
    } finally {
      reloading = false;
      if (pending && !closed) {
        pending = false;
        schedule();
      }
    }
  }

  function schedule() {
    if (closed) return;
    if (timer) clearTimeout(timer);
    timer = setTimeout(() => { timer = null; void run(); }, RELOAD_DELAY_MS);
  }

  for (const root of packageRoots(config)) {
    const dir = path.join(root, 'plugins');
    if (fs.existsSync(dir)) arm(dir, true);
  }
  for (const entry of config.plugins || []) {
    if (typeof entry !== 'string' || !entry || /^[a-z]+:/i.test(entry)) continue;
    const filename = path.resolve(config.configDirectory || process.cwd(), entry);
    if (!fs.existsSync(filename)) continue;
    const dir = path.dirname(filename);
    if (covered(dir)) continue;
    arm(filename, false);
    arm(dir, false);
  }
  if (watchers.length) console.error('[plugins] 正在监视插件变更');

  return {
    close() {
      closed = true;
      if (timer) clearTimeout(timer);
      timer = null;
      for (const watcher of watchers) {
        try { watcher.close(); } catch { /* 已经关掉。 */ }
      }
      watchers.length = 0;
    },
  };
}
