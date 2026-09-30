import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';

export const DEFAULT_LOCALE = 'zh-CN';
export function validateLocale(locale) {
  if (locale !== 'zh-CN' && locale !== 'en') throw new Error('语言仅支持 zh-CN 或 en');
  return locale;
}
export function loadLanguageSettings(stateDir, fallback = DEFAULT_LOCALE) {
  const initial = validateLocale(fallback);
  if (!stateDir) return initial;
  try {
    const file = path.join(stateDir, 'language-settings.json');
    if (!fs.lstatSync(file).isFile()) throw new Error('语言设置文件无效');
    const saved = JSON.parse(fs.readFileSync(file, 'utf8'));
    return validateLocale(saved?.locale);
  } catch (error) {
    if (error.code === 'ENOENT') return initial;
    throw error;
  }
}
export function saveLanguageSettings(config, payload) {
  if (!payload || typeof payload !== 'object' || Array.isArray(payload) ||
      Object.keys(payload).some(key => key !== 'locale')) throw new Error('语言设置格式无效');
  const locale = validateLocale(payload.locale);
  const directory = config.stateDir;
  fs.mkdirSync(directory, { recursive: true, mode: 0o700 });
  const file = path.join(directory, 'language-settings.json');
  const temporary = path.join(directory, `.language-settings-${process.pid}-${randomUUID()}`);
  try {
    fs.writeFileSync(temporary, JSON.stringify({ locale }), { flag: 'wx', mode: 0o600 });
    fs.renameSync(temporary, file);
  } finally { fs.rmSync(temporary, { force: true }); }
  // Only update the live locale after the durable write succeeds.
  config.locale = locale;
  return { locale };
}
