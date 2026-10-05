import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';

export const DEFAULT_TRANSCRIBE_URL = 'https://api.openai.com/v1/audio/transcriptions';
export const DEFAULT_TRANSCRIBE_MODEL = 'gpt-transcribe';

export function validateTranscribeUrl(value) {
  if (typeof value !== 'string' || value.length < 1 || value.length > 2048 || value !== value.trim()) {
    throw new Error('语音接口 URL 无效');
  }
  let url;
  try { url = new URL(value); }
  catch { throw new Error('语音接口 URL 无效'); }
  const loopback = ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname.toLowerCase());
  if (url.protocol !== 'https:' && !(url.protocol === 'http:' && loopback)) {
    throw new Error('语音接口必须使用 HTTPS；仅本机代理允许 HTTP');
  }
  if (url.username || url.password || url.hash || !url.pathname || url.pathname === '/') {
    throw new Error('语音接口 URL 不能包含账号、密码或片段，且须包含接口路径');
  }
  return url.href;
}

export function validateTranscribeModel(value) {
  if (typeof value !== 'string' || !/^[A-Za-z0-9][A-Za-z0-9._:/@-]{0,127}$/.test(value)) {
    throw new Error('语音模型名称无效');
  }
  return value;
}

function settingsPath(stateDir) {
  return path.join(stateDir, 'secrets', 'voice-settings.json');
}

export function loadVoiceSettings(stateDir) {
  const file = settingsPath(stateDir);
  try {
    if (!fs.lstatSync(file).isFile()) throw new Error('语音设置文件无效');
    const saved = JSON.parse(fs.readFileSync(file, 'utf8'));
    return {
      url: validateTranscribeUrl(saved.url),
      model: validateTranscribeModel(saved.model),
    };
  } catch (error) {
    if (error.code === 'ENOENT') return null;
    throw error;
  }
}

export function saveVoiceSettings(stateDir, settings) {
  const normalized = {
    url: validateTranscribeUrl(settings.url),
    model: validateTranscribeModel(settings.model),
  };
  const directory = path.join(stateDir, 'secrets');
  const file = settingsPath(stateDir);
  fs.mkdirSync(directory, { recursive: true, mode: 0o700 });
  fs.chmodSync(directory, 0o700);
  const temporary = path.join(directory, `.voice-settings-${process.pid}-${randomUUID()}`);
  try {
    fs.writeFileSync(temporary, JSON.stringify(normalized), { flag: 'wx', mode: 0o600 });
    fs.renameSync(temporary, file);
    fs.chmodSync(file, 0o600);
  } finally {
    fs.rmSync(temporary, { force: true });
  }
  return normalized;
}
