import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { builtinAgent } from './agents/index.mjs';

export const VIBE_SESSION_ID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
export const CLI_SESSION_ID = /^[A-Za-z0-9_-]{6,128}$/;
const PROVIDER_ID = /^[a-z0-9][a-z0-9_-]{0,39}$/;
const OPAQUE_SESSION_ID = /^[A-Za-z0-9][A-Za-z0-9_.:-]{0,255}$/;

export function validAgentSessionId(plugin, value) {
  if (typeof value !== 'string' || value.length > 256) return false;
  const pattern = plugin?.sessionIdPattern || OPAQUE_SESSION_ID;
  pattern.lastIndex = 0;
  return pattern.test(value);
}
const DEFAULT_TITLE = '新任务';

function now() { return new Date().toISOString(); }

function titleFrom(value) {
  if (typeof value !== 'string') throw new Error('任务标题必须是文字');
  const title = value.replace(/[\x00-\x1f\x7f]+/g, ' ').trim();
  if (!title || Array.from(title).length > 60) throw new Error('任务标题需在 1 到 60 字之间');
  return title;
}

function firstTurnTitle(instruction) {
  const compact = String(instruction).replace(/\s+/g, ' ').trim();
  const chars = Array.from(compact);
  return chars.slice(0, 24).join('') + (chars.length > 24 ? '…' : '');
}

export class SessionStore {
  constructor({ file, projects, defaultProject, defaultProvider, pluginsRuntime, agent }) {
    this.agent = agent || (pluginsRuntime ? (id) => pluginsRuntime.agent(id) : builtinAgent);
    this.file = file;
    this.projects = projects;
    this.defaultProject = defaultProject;
    this.defaultProvider = defaultProvider;
    fs.mkdirSync(path.dirname(file), { recursive: true, mode: 0o700 });
    const parsed = fs.existsSync(file) ? JSON.parse(fs.readFileSync(file, 'utf8')) : [];
    this.sessions = Array.isArray(parsed) ? parsed : [];
    let migrated = false;
    for (const session of this.sessions) {
      if (session.autoTitlePending && session.title === '新会话') {
        session.title = DEFAULT_TITLE;
        migrated = true;
      }
    }
    if (migrated) this.save();
  }

  save() {
    const temp = `${this.file}.${process.pid}.tmp`;
    fs.writeFileSync(temp, JSON.stringify(this.sessions, null, 2), { mode: 0o600 });
    fs.renameSync(temp, this.file);
  }

  list(provider) {
    return this.sessions.filter((session) => !provider || session.provider === provider)
      .sort((a, b) => b.updatedAt.localeCompare(a.updatedAt));
  }

  get(id) { return this.sessions.find((session) => session.id === id); }

  create({ provider, projectId, title, legacy = false, cliSessionId = '' } = {}) {
    const selectedProvider = provider || this.defaultProvider;
    const selectedProject = projectId || this.defaultProject;
    const plugin = this.agent(selectedProvider);
    if (!plugin && !(legacy && PROVIDER_ID.test(selectedProvider))) throw new Error('不支持的编程工具');
    if (!this.projects.some((project) => project.id === selectedProject)) {
      throw new Error('项目不在本机允许列表中');
    }
    const explicitTitle = title !== undefined && title !== null;
    const timestamp = now();
    const session = {
      id: randomUUID(), provider: selectedProvider, projectId: selectedProject,
      title: explicitTitle ? titleFrom(title) : DEFAULT_TITLE,
      autoTitlePending: !explicitTitle,
      contextMode: plugin?.capabilities.session || 'native',
      cliSessionId: validAgentSessionId(plugin, cliSessionId) ? cliSessionId : '',
      legacy, createdAt: timestamp, updatedAt: timestamp,
    };
    this.sessions.push(session);
    this.save();
    return session;
  }

  rename(id, title) {
    const session = this.get(id);
    if (!session) throw new Error('任务不存在');
    session.title = titleFrom(title);
    session.autoTitlePending = false;
    session.updatedAt = now();
    this.save();
    return session;
  }

  delete(id) {
    const index = this.sessions.findIndex((session) => session.id === id);
    if (index < 0) throw new Error('任务不存在');
    this.sessions.splice(index, 1);
    this.save();
  }

  recordTurn(id, instruction) {
    const session = this.get(id);
    if (!session) throw new Error('任务不存在');
    if (session.autoTitlePending) {
      session.title = firstTurnTitle(instruction);
      session.autoTitlePending = false;
    }
    session.updatedAt = now();
    this.save();
    return session;
  }

  setCliSession(id, cliSessionId) {
    const session = this.get(id);
    if (!session || !validAgentSessionId(this.agent(session.provider), cliSessionId)) return;
    session.cliSessionId = cliSessionId;
    session.updatedAt = now();
    this.save();
  }

  setGitSnapshot(id, snapshot) {
    const session = this.get(id);
    if (!session || typeof snapshot !== 'string') return;
    session.gitSnapshot = snapshot;
    session.updatedAt = now();
    this.save();
  }
}
