import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';

function secretPaths(stateDir) {
  const directory = path.join(stateDir, 'secrets');
  return { directory, file: path.join(directory, 'openai-api-key') };
}

export function loadOpenAIKey(stateDir) {
  const { file } = secretPaths(stateDir);
  try {
    if (!fs.lstatSync(file).isFile()) throw new Error('密钥存储文件无效');
    const key = fs.readFileSync(file, 'utf8').trim();
    if (!key || key.length > 512 || /\s/.test(key)) throw new Error('本机保存的 OpenAI 密钥格式无效');
    return key;
  } catch (error) {
    if (error.code === 'ENOENT') return '';
    throw error;
  }
}

export function validateOpenAIKey(value) {
  if (typeof value !== 'string' || value.length > 512 || (value && (value !== value.trim() || /\s/.test(value)))) {
    throw new Error('语音 API Key 格式无效');
  }
  return value;
}

export function saveOpenAIKey(stateDir, value) {
  validateOpenAIKey(value);
  const { directory, file } = secretPaths(stateDir);
  if (value === '') {
    fs.rmSync(file, { force: true });
    return;
  }
  fs.mkdirSync(directory, { recursive: true, mode: 0o700 });
  fs.chmodSync(directory, 0o700);
  const temporary = path.join(directory, `.openai-api-key-${process.pid}-${randomUUID()}`);
  try {
    fs.writeFileSync(temporary, value, { flag: 'wx', mode: 0o600 });
    fs.renameSync(temporary, file);
    fs.chmodSync(file, 0o600);
  } finally {
    fs.rmSync(temporary, { force: true });
  }
}
