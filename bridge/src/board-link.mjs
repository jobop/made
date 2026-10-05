export const CAPABILITY_NAME = /^[a-z][a-z0-9_]{0,15}(?:\.[a-z][a-z0-9_]{0,15}){0,2}$/;
const FIELD_KEY = /^[a-z][a-z0-9_]{0,15}$/;
const MAX_ITEMS = 32;
const MAX_COMMANDS = 8;
const MAX_FIELDS = 6;
const MAX_TEXT = 120;
const MAX_PER_SECOND = 8;
const PLUGIN_TIMEOUT_MS = 1500;

function fail(message) {
  const error = new Error(message);
  error.preserveMessage = true;
  return error;
}

function readNames(list, { cadence }) {
  if (!Array.isArray(list) || list.length > MAX_ITEMS) throw fail('能力清单无效');
  const items = [];
  const seen = new Set();
  for (const item of list) {
    if (!item || typeof item !== 'object' || Array.isArray(item)) throw fail('能力清单无效');
    if (typeof item.name !== 'string' || !CAPABILITY_NAME.test(item.name) || seen.has(item.name)) throw fail('能力清单无效');
    const rate = item.cadence || 'edge';
    if (cadence && rate !== 'edge' && rate !== 'continuous') throw fail('能力清单无效');
    if (!cadence && item.cadence !== undefined && item.cadence !== 'edge') throw fail('能力清单无效');
    seen.add(item.name);
    items.push(cadence ? { name: item.name, cadence: rate } : { name: item.name });
  }
  return items;
}

export function parseCatalog(payload) {
  if (!payload || typeof payload !== 'object' || Array.isArray(payload)) throw fail('能力清单无效');
  return {
    events: readNames(payload.events, { cadence: true }),
    commands: readNames(payload.commands, { cadence: false }),
  };
}

export function parseFields(fields) {
  if (fields === undefined || fields === null) return {};
  if (typeof fields !== 'object' || Array.isArray(fields)) throw fail('事件字段无效');
  const keys = Object.keys(fields);
  if (keys.length > MAX_FIELDS) throw fail('事件字段无效');
  const out = {};
  for (const key of keys) {
    const value = fields[key];
    if (!FIELD_KEY.test(key) || typeof value !== 'string' || Buffer.byteLength(value) > MAX_TEXT) throw fail('事件字段无效');
    out[key] = value;
  }
  return out;
}

function pluginAvailable(plugin, config) {
  try {
    const result = plugin.probe(config);
    return Boolean(result && typeof result === 'object' && !(result instanceof Promise) && result.available === true);
  } catch {
    return false;
  }
}

function commandsFrom(plugin, result, catalog) {
  if (!result || typeof result !== 'object' || !Array.isArray(result.commands)) return [];
  const allowed = new Set(plugin.commands);
  const known = new Set(catalog.commands.map((item) => item.name));
  const commands = [];
  for (const command of result.commands) {
    if (!command || typeof command !== 'object' || !allowed.has(command.name) || !known.has(command.name)) continue;
    let fields;
    try { fields = parseFields(command.fields); } catch { continue; }
    commands.push({ name: command.name, fields });
  }
  return commands;
}

async function runPlugin(plugin, context) {
  let timer;
  try {
    return await Promise.race([
      Promise.resolve(plugin.onEvent(context)),
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(fail('插件处理超时')), PLUGIN_TIMEOUT_MS);
      }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

export function createBoardLink() {
  const catalogs = new Map();
  const hits = new Map();
  return {
    setCatalog(deviceId, payload) {
      const catalog = parseCatalog(payload);
      catalogs.set(deviceId, catalog);
      return catalog;
    },
    subscription(deviceId, runtime, config) {
      const catalog = catalogs.get(deviceId);
      if (!catalog || !runtime?.boardPlugins) return [];
      const known = new Set(catalog.events.map((item) => item.name));
      const wanted = [];
      for (const plugin of runtime.boardPlugins()) {
        if (!pluginAvailable(plugin, config)) continue;
        for (const name of plugin.events) {
          if (known.has(name) && !wanted.includes(name)) wanted.push(name);
        }
      }
      return wanted;
    },
    async handleEvent(deviceId, payload, runtime, config) {
      if (!payload || typeof payload !== 'object' || Array.isArray(payload)) throw fail('事件无效');
      if (typeof payload.name !== 'string' || !CAPABILITY_NAME.test(payload.name)) throw fail('事件无效');
      const fields = parseFields(payload.fields);
      const catalog = catalogs.get(deviceId);
      if (!catalog || !catalog.events.some((item) => item.name === payload.name)) throw fail('事件不在板侧清单');
      const subscribed = this.subscription(deviceId, runtime, config);
      if (!subscribed.includes(payload.name)) throw fail('事件未注册');
      const key = `${deviceId}\n${payload.name}`;
      const now = Date.now();
      const recent = (hits.get(key) || []).filter((at) => now - at < 1000);
      if (recent.length >= MAX_PER_SECOND) throw fail('事件上报过于频繁');
      recent.push(now);
      hits.set(key, recent);
      const commands = [];
      for (const plugin of runtime.boardPlugins()) {
        if (!plugin.events.includes(payload.name) || !pluginAvailable(plugin, config)) continue;
        let result;
        try {
          result = await runPlugin(plugin, { event: { name: payload.name, fields }, config, deviceId });
        } catch {
          continue;
        }
        try {
          commands.push(...commandsFrom(plugin, result, catalog));
        } catch {
          continue;
        }
        if (commands.length >= MAX_COMMANDS) break;
      }
      return { commands: commands.slice(0, MAX_COMMANDS) };
    },
  };
}
