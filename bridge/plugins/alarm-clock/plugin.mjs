const ID = 'alarm-clock';
const beep = beepWav();
const timers = new Map();
const ringing = new Map();

function beepWav() {
  const rate = 16000;
  const samples = Math.floor(rate * 0.7);
  const data = Buffer.alloc(samples * 2);
  for (let i = 0; i < samples; i += 1) {
    const t = i / rate;
    const freq = Math.floor(t * 5) % 2 === 0 ? 880 : 587;
    const env = Math.min(1, i / 200, (samples - i) / 400);
    const value = Math.round(Math.sin(2 * Math.PI * freq * t) * 11000 * env);
    data.writeInt16LE(value, i * 2);
  }
  const header = Buffer.alloc(44);
  header.write('RIFF', 0);
  header.writeUInt32LE(36 + data.length, 4);
  header.write('WAVE', 8);
  header.write('fmt ', 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(1, 22);
  header.writeUInt32LE(rate, 24);
  header.writeUInt32LE(rate * 2, 28);
  header.writeUInt16LE(2, 32);
  header.writeUInt16LE(16, 34);
  header.write('data', 36);
  header.writeUInt32LE(data.length, 40);
  return Buffer.concat([header, data]);
}

function settingsOf(config) {
  const raw = config?.pluginSettings?.[ID];
  return raw && typeof raw === 'object' && !Array.isArray(raw) ? raw : {};
}

function labelOf(config) {
  const label = settingsOf(config).label;
  return typeof label === 'string' && label.trim() ? label.trim() : '闹钟';
}

function clearTimer(deviceId) {
  const timer = timers.get(deviceId);
  if (timer) clearTimeout(timer);
  timers.delete(deviceId);
}

function markRinging(deviceId) {
  const previous = ringing.get(deviceId);
  if (previous) clearTimeout(previous);
  const timer = setTimeout(() => ringing.delete(deviceId), 60_000);
  timer.unref?.();
  ringing.set(deviceId, timer);
}

function schedule(deviceId, send, delayMs, text, again) {
  clearTimer(deviceId);
  const timer = setTimeout(() => {
    timers.delete(deviceId);
    markRinging(deviceId);
    try {
      send({ name: 'caption.show', fields: { text } });
      send({ name: 'audio.play' }, beep);
    } catch (error) {
      console.error(`闹钟 ${deviceId}：${error.message}`);
    }
    if (typeof again === 'function') again();
  }, delayMs);
  timer.unref?.();
  timers.set(deviceId, timer);
}

function delayUntil(time) {
  const [hour, minute] = time.split(':').map(Number);
  const now = new Date();
  const next = new Date(now);
  next.setHours(hour, minute, 0, 0);
  if (next <= now) next.setDate(next.getDate() + 1);
  return next.getTime() - now.getTime();
}

function plan(deviceId, send, config) {
  const settings = settingsOf(config);
  const label = labelOf(config);
  const again = () => plan(deviceId, send, config);
  if (typeof settings.demoSeconds === 'string' && /^[1-9]\d{0,3}$/.test(settings.demoSeconds)) {
    schedule(deviceId, send, Number(settings.demoSeconds) * 1000, label, again);
    return;
  }
  if (typeof settings.time === 'string' && /^([01]\d|2[0-3]):[0-5]\d$/.test(settings.time)) {
    schedule(deviceId, send, delayUntil(settings.time), label, again);
    return;
  }
  clearTimer(deviceId);
}

export default {
  apiVersion: 1,
  kind: 'board-plugin',
  id: ID,
  label: '闹钟',
  events: ['boot.double', 'boot.triple'],
  commands: ['caption.show', 'audio.play'],
  probe: () => ({ available: true }),
  onConnect({ deviceId, send, config }) {
    plan(deviceId, send, config);
  },
  onEvent({ event, deviceId, send, config }) {
    if (event.name === 'boot.triple') {
      clearTimer(deviceId);
      ringing.delete(deviceId);
      return { commands: [{ name: 'caption.show', fields: { text: '闹钟已关' } }] };
    }
    if (event.name === 'boot.double' && (timers.has(deviceId) || ringing.has(deviceId))) {
      schedule(deviceId, send, 60_000, labelOf(config), () => plan(deviceId, send, config));
      return { commands: [{ name: 'caption.show', fields: { text: '一分钟后再响' } }] };
    }
    return { commands: [] };
  },
};
