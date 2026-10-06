export const CAPABILITY_NAME = /^[a-z][a-z0-9_]{0,15}(?:\.[a-z][a-z0-9_]{0,15}){0,2}$/;
const FIELD_KEY = /^[a-z][a-z0-9_]{0,15}$/;
const COMMAND_ID = /^[1-9][0-9]{0,15}$/;
const MAX_ITEMS = 32;
const MAX_QUEUE = 8;
const MAX_FIELDS = 6;
const MAX_TEXT = 120;
const MAX_PER_SECOND = 8;
const MAX_AUDIO = 256 * 1024;
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

function pcmWav(audio) {
  if (!Buffer.isBuffer(audio) || audio.length < 44 || audio.length > MAX_AUDIO) return false;
  if (audio.toString('ascii', 0, 4) !== 'RIFF' || audio.toString('ascii', 8, 12) !== 'WAVE') return false;
  let offset = 12;
  let format = 0;
  let channels = 0;
  let rate = 0;
  let bits = 0;
  let data = 0;
  while (offset + 8 <= audio.length) {
    const id = audio.toString('ascii', offset, offset + 4);
    const size = audio.readUInt32LE(offset + 4);
    if (id === 'fmt ' && offset + 24 <= audio.length) {
      format = audio.readUInt16LE(offset + 8);
      channels = audio.readUInt16LE(offset + 10);
      rate = audio.readUInt32LE(offset + 12);
      bits = audio.readUInt16LE(offset + 22);
    } else if (id === 'data') data = size;
    const step = 8 + size + (size & 1);
    if (step > audio.length) return false;
    offset += step;
  }
  return format === 1 && bits === 16 && channels >= 1 && channels <= 2 && rate >= 8000 && rate <= 48000 && data > 0;
}

function enqueueCommand(queues, audios, catalogs, seq, deviceId, command, allowed, audio) {
  const catalog = catalogs.get(deviceId);
  if (!catalog) throw fail('板侧尚未上报命令清单');
  if (!command || typeof command !== 'object' || typeof command.name !== 'string' || !CAPABILITY_NAME.test(command.name)) throw fail('命令无效');
  if (!catalog.commands.some((item) => item.name === command.name)) throw fail('命令不在板侧清单');
  if (allowed && !allowed.has(command.name)) throw fail('插件未声明该命令');
  const fields = parseFields(command.fields);
  if (audio != null && command.name !== 'audio.play') throw fail('只有 audio.play 可以附带语音');
  if (command.name === 'audio.play' && audio == null) throw fail('audio.play 需要同时附带语音');
  if (audio != null && !pcmWav(audio)) throw fail('语音须为不超过 256KB 的 16-bit PCM WAV');
  const queue = queues.get(deviceId) || [];
  if (queue.length >= MAX_QUEUE) throw fail('命令队列已满');
  if (audio != null && queue.some((item) => item.audio)) throw fail('已有一条待播放语音');
  seq.n += 1;
  const item = { id: String(seq.n), name: command.name, fields, audio: audio != null };
  queue.push(item);
  queues.set(deviceId, queue);
  if (audio != null) audios.set(`${deviceId}\n${item.id}`, audio);
  return { id: item.id, name: item.name, fields: { ...item.fields }, audio: item.audio };
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
  const queues = new Map();
  const audios = new Map();
  const hits = new Map();
  const seq = { n: 0 };
  const dropAudio = (deviceId, id) => audios.delete(`${deviceId}\n${id}`);
  const enqueue = (deviceId, command, allowed, audio) => enqueueCommand(queues, audios, catalogs, seq, deviceId, command, allowed, audio);
  return {
    setCatalog(deviceId, payload) {
      const catalog = parseCatalog(payload);
      catalogs.set(deviceId, catalog);
      const known = new Set(catalog.commands.map((item) => item.name));
      const queue = queues.get(deviceId) || [];
      const next = queue.filter((item) => known.has(item.name));
      for (const item of queue) {
        if (!next.includes(item)) dropAudio(deviceId, item.id);
      }
      queues.set(deviceId, next);
      return catalog;
    },
    forget(deviceId) {
      catalogs.delete(deviceId);
      queues.delete(deviceId);
      for (const key of [...hits.keys(), ...audios.keys()]) {
        if (key.startsWith(`${deviceId}\n`)) {
          hits.delete(key);
          audios.delete(key);
        }
      }
    },
    pending(deviceId) {
      return (queues.get(deviceId) || []).map((item) => ({
        id: item.id, name: item.name, fields: { ...item.fields }, audio: item.audio === true,
      }));
    },
    audio(deviceId, id) {
      const wav = audios.get(`${deviceId}\n${id}`);
      return wav ? Buffer.from(wav) : null;
    },
    send(deviceId, command, audio) {
      return enqueue(deviceId, command, undefined, audio);
    },
    deviceIds() {
      return [...catalogs.keys()];
    },
    connect(deviceId, runtime, config) {
      if (!runtime?.boardPlugins) return;
      for (const plugin of runtime.boardPlugins()) {
        if (typeof plugin.onConnect !== 'function' || !pluginAvailable(plugin, config)) continue;
        const allowed = new Set(plugin.commands);
        const send = (command, audio) => enqueue(deviceId, command, allowed, audio);
        try {
          const result = plugin.onConnect({ deviceId, config, send });
          if (result && typeof result.then === 'function') result.catch(() => {});
        } catch {
          /* A plugin that fails to arm must not block the catalog response. */
        }
      }
    },
    ack(deviceId, payload) {
      if (!payload || typeof payload !== 'object' || Array.isArray(payload) || !Array.isArray(payload.ids) || payload.ids.length > MAX_QUEUE) throw fail('确认无效');
      const drop = new Set();
      for (const id of payload.ids) {
        if (typeof id !== 'string' || !COMMAND_ID.test(id) || drop.has(id)) throw fail('确认无效');
        drop.add(id);
      }
      const queue = queues.get(deviceId) || [];
      const next = queue.filter((item) => !drop.has(item.id));
      for (const id of drop) dropAudio(deviceId, id);
      queues.set(deviceId, next);
      return { acked: queue.length - next.length };
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
      const allowedFor = (plugin) => new Set(plugin.commands);
      for (const plugin of runtime.boardPlugins()) {
        if (!plugin.events.includes(payload.name) || !pluginAvailable(plugin, config)) continue;
        const allowed = allowedFor(plugin);
        let result;
        try {
          result = await runPlugin(plugin, {
            event: { name: payload.name, fields },
            config,
            deviceId,
            send: (command, audio) => enqueue(deviceId, command, allowed, audio),
          });
        } catch {
          continue;
        }
        for (const command of commandsFrom(plugin, result, catalog)) {
          try {
            enqueue(deviceId, command, allowed);
          } catch (error) {
            if (error.message === '命令队列已满') break;
          }
        }
      }
      return { accepted: true };
    },
  };
}
