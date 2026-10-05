import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';

export const DEFAULT_CODEX_MODEL = 'gpt-6-astra';
const DEFAULT_MODEL_AGENTS = [
  { id: 'codex', capabilities: {model: true}, model: {required: true} },
  { id: 'cursor', capabilities: {model: true} }, { id: 'qoder', capabilities: {model: true} },
];
export const MODEL_ID = /^[A-Za-z0-9][A-Za-z0-9._:/+@\[\],=-]{0,119}$/;

export function validateModelSettings(value, registry) {
  const agents = (registry?.agents() || DEFAULT_MODEL_AGENTS).filter(p => p.capabilities.model);
  if (!value || typeof value !== 'object' || Array.isArray(value) ||
      Object.keys(value).length !== agents.length || !agents.every(p => Object.hasOwn(value, p.id))) {
    throw new Error('请填写当前已安装助手的模型设置');
  }
  return validateStoredModels(value, new Set(agents.filter(p => p.model?.required).map(p => p.id)));
}

export function validateStoredModels(value, required = new Set()) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('模型设置格式无效');
  const models = {};
  for (const [provider, model] of Object.entries(value)) {
    if (!/^[a-z0-9][a-z0-9_-]{0,39}$/.test(provider) || ['constructor','prototype','__proto__'].includes(provider) ||
        typeof model !== 'string' || (!model && required.has(provider)) || (model && !MODEL_ID.test(model))) {
      throw new Error(`${provider} 模型名称无效`);
    }
    models[provider] = model;
  }
  return models;
}

function settingsPath(stateDir) {
  return path.join(stateDir, 'models.json');
}

export function loadModelSettings(stateDir) {
  const file = settingsPath(stateDir);
  try {
    if (!fs.lstatSync(file).isFile()) throw new Error('模型设置文件无效');
    return validateStoredModels(JSON.parse(fs.readFileSync(file, 'utf8')));
  } catch (error) {
    if (error.code === 'ENOENT') return null;
    throw error;
  }
}

export function saveModelSettings(stateDir, value, registry) {
  const validated = validateModelSettings(value, registry);
  const models = { ...(loadModelSettings(stateDir) || {}), ...validated };
  fs.mkdirSync(stateDir, { recursive: true, mode: 0o700 });
  const file = settingsPath(stateDir);
  const temporary = path.join(stateDir, `.models-${process.pid}-${randomUUID()}`);
  try {
    fs.writeFileSync(temporary, JSON.stringify(models), { flag: 'wx', mode: 0o600 });
    fs.renameSync(temporary, file);
    fs.chmodSync(file, 0o600);
  } finally {
    fs.rmSync(temporary, { force: true });
  }
  return models;
}
