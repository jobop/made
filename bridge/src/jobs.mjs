import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import { SessionStore, validAgentSessionId, VIBE_SESSION_ID } from './sessions.mjs';
import { builtinAgent } from './agents/index.mjs';

const ACTIVE = new Set(['waiting_confirmation', 'queued', 'running']);

function now() {
  return new Date().toISOString();
}

export class JobStore {
  constructor({ file, projects, defaultProject, defaultProvider, workspaceRoot, run, providerAvailable, pluginsRuntime }) {
    this.agent = pluginsRuntime ? (id) => pluginsRuntime.agent(id) : run.agent || builtinAgent;
    this.file = file;
    this.projects = projects;
    this.workspaceRoot = workspaceRoot || '';
    this.defaultProject = defaultProject;
    this.defaultProvider = defaultProvider;
    this.run = run;
    this.providerAvailable = providerAvailable;
    this.sessions = new SessionStore({
      file: path.join(path.dirname(file), 'sessions.json'), projects, defaultProject, defaultProvider, agent: this.agent,
    });
    this.jobs = [];
    this.controllers = new Map();
    this.progressSavedAt = new Map();
    this.pumping = false;
    this.handoffRefresh = null;
    this.nextHandoffPollAt = 0;
    this.handoffIndex = 0;
    this.load();
  }

  load() {
    fs.mkdirSync(path.dirname(this.file), { recursive: true, mode: 0o700 });
    if (fs.existsSync(this.file)) {
      const parsed = JSON.parse(fs.readFileSync(this.file, 'utf8'));
      this.jobs = Array.isArray(parsed) ? parsed : [];
      for (const job of this.jobs) {
        if (!job.vibeSessionId || !this.sessions.get(job.vibeSessionId)) {
          const session = this.sessions.create({
            provider: job.provider, projectId: job.projectId, legacy: true,
            cliSessionId: job.sessionId || '',
          });
          this.sessions.recordTurn(session.id, job.instruction || '历史任务');
          job.vibeSessionId = session.id;
        }
        if (job.status === 'running' || job.status === 'queued') {
          job.status = 'failed';
          job.error = '桥接客户端重启，任务执行已中断。请检查代码目录后重新提交。';
          job.updatedAt = now();
        }
        this.run.syncJobState?.(job);
        if (job.status === 'handed_off' && job.handoff?.readState === 'unavailable') {
          // A fresh process may have newly authorized credentials.
          job.handoff.readState = 'retrying';
        }
      }
      this.save();
    }
  }

  save() {
    for (const job of this.jobs) this.run.syncJobState?.(job);
    const temp = `${this.file}.${process.pid}.tmp`;
    fs.writeFileSync(temp, JSON.stringify(this.jobs, null, 2), { mode: 0o600 });
    fs.renameSync(temp, this.file);
  }

  list() {
    // Both the desktop and the device read through this method. A throttled
    // refresh makes handed-off replies available to both without a UI timer.
    void this.refreshHandoffs();
    return [...this.jobs].reverse();
  }

  refreshHandoffs({ force = false } = {}) {
    if (typeof this.run.readUpdates !== 'function') return Promise.resolve();
    if (this.handoffRefresh) return this.handoffRefresh;
    if (!force && Date.now() < this.nextHandoffPollAt) return Promise.resolve();
    const jobs = this.jobs.filter((job) =>
      job.status === 'handed_off' && job.handoff?.cursor &&
      !['unavailable', 'ambiguous'].includes(job.handoff.readState) &&
      (this.run.canReadUpdates?.(job) ?? true)
    ).reverse().slice(0, 8);
    if (!jobs.length) return Promise.resolve();
    const job = jobs[this.handoffIndex % jobs.length];
    this.handoffIndex += 1;
    this.nextHandoffPollAt = Date.now() + 8000;
    this.handoffRefresh = this.pollHandoff(job).finally(() => {
      this.handoffRefresh = null;
    });
    return this.handoffRefresh;
  }

  async pollHandoff(job) {
    try {
      const update = await this.run.readUpdates(job);
      if (job.status !== 'handed_off' || !update) return;
      let changed = false;
      if (update.handoff && typeof update.handoff === 'object') {
        job.handoff = { ...job.handoff, ...update.handoff };
        changed = true;
      }
      if (typeof update.appendResult === 'string' && update.appendResult) {
        job.result = `${job.result}\n\n${update.appendResult}`.slice(-12000);
        changed = true;
      }
      if (changed) {
        delete job.handoffReadError;
        job.updatedAt = now();
        this.save();
      }
    } catch (error) {
      // Reading a reply never undoes a successful dispatch.
      if (job.status === 'handed_off') {
        job.handoff = { ...job.handoff, readState: 'retrying' };
        job.handoffReadError = error instanceof Error ? error.message : String(error);
        this.save();
      }
    }
  }

  get(id) {
    return id ? this.jobs.find((job) => job.id === id) : this.jobs.at(-1);
  }

  listSessions(provider) {
    return this.sessions.list(provider).map((session) => {
      const latest = [...this.jobs].reverse().find((job) => job.vibeSessionId === session.id);
      return { ...session, status: latest?.status || 'empty' };
    });
  }

  createSession(payload) {
    const session = this.sessions.create(payload);
    return { ...session, status: 'empty' };
  }

  renameSession(id, title) {
    this.sessions.rename(id, title);
    return this.listSessions().find((session) => session.id === id);
  }

  deleteSession(id) {
    if (!VIBE_SESSION_ID.test(id || '') || !this.sessions.get(id)) throw new Error('任务不存在');
    const turns = this.jobs.filter((job) => job.vibeSessionId === id);
    if (turns.some((job) => job.status === 'running' && !this.agent(job.provider)?.capabilities.cancel)) {
      throw new Error('这个编程工具不支持撤回正在执行的任务，请先在工具中结束执行');
    }
    for (const job of turns) {
      if (ACTIVE.has(job.status)) this.cancel(job.id);
      // A handoff poll or an agent that finishes after abort must not write a
      // removed turn back into the bridge's task history.
      job.status = 'deleted';
      this.progressSavedAt.delete(job.id);
    }
    this.jobs = this.jobs.filter((job) => job.vibeSessionId !== id);
    // Persist jobs first: a crash between files leaves an empty session,
    // rather than an orphan turn that load() would recreate as a new session.
    this.save();
    this.sessions.delete(id);
    return { id, deleted: true };
  }

  submit({ instruction, sessionId, provider, projectId, source = 'voice' }) {
    const text = String(instruction || '').trim();
    if (!VIBE_SESSION_ID.test(sessionId || '')) throw new Error('请先新建任务');
    const session = this.sessions.get(sessionId);
    if (!session) throw new Error('任务不存在');
    const selectedProvider = provider || session.provider;
    const selectedProject = projectId || session.projectId;
    if (selectedProvider !== session.provider || selectedProject !== session.projectId) {
      throw new Error('任务与编程工具或项目不匹配');
    }
    if (text.length < 3 || text.length > 2000) {
      throw new Error('任务内容需在 3 到 2000 字之间');
    }
    if (!this.agent(selectedProvider)) {
      throw new Error('不支持的编程工具');
    }
    if (!this.projects.some((project) => project.id === selectedProject)) {
      throw new Error('项目不在本机允许列表中');
    }
    if (this.jobs.some((item) => item.vibeSessionId === session.id && ACTIVE.has(item.status))) {
      throw new Error('请先完成或取消当前任务的上一条指令');
    }
    if (session.contextMode === 'native' &&
        this.jobs.some((item) => item.vibeSessionId === session.id && item.status === 'completed') &&
        !session.cliSessionId) {
      throw new Error('编程工具未返回可恢复的会话 ID，无法继续此任务；请新建任务');
    }
    const job = {
      id: randomUUID().slice(0, 12),
      vibeSessionId: session.id,
      sessionId: session.cliSessionId || undefined,
      instruction: text,
      provider: selectedProvider,
      projectId: selectedProject,
      source,
      status: 'waiting_confirmation',
      createdAt: now(),
      updatedAt: now(),
      result: '',
      progress: '',
      progressAt: '',
      error: '',
    };
    this.sessions.recordTurn(session.id, text);
    this.jobs.push(job);
    this.save();
    return job;
  }

  confirm(id) {
    const job = this.get(id);
    if (!job) throw new Error('任务不存在');
    if (job.status !== 'waiting_confirmation') throw new Error('当前任务不能确认');
    const capability = this.providerAvailable(job.provider);
    if (!capability?.available) {
      const error = new Error(capability?.reason || '编程工具尚未就绪');
      if (capability?.preserveMessage && capability.reason) error.preserveMessage = true;
      throw error;
    }
    job.error = '';
    job.progress = '';
    job.progressAt = '';
    delete job.retryPending;
    job.status = 'queued';
    job.updatedAt = now();
    this.save();
    void this.pump();
    return job;
  }

  cancel(id) {
    const job = this.get(id);
    if (!job) throw new Error('任务不存在');
    if (!ACTIVE.has(job.status)) throw new Error('当前任务不能取消');
    if (job.status === 'running' && !this.agent(job.provider)?.capabilities.cancel) {
      throw new Error('这个编程工具不支持撤回正在执行的任务，请在工具中处理');
    }
    job.status = 'cancelled';
    delete job.retryPending;
    job.updatedAt = now();
    this.controllers.get(job.id)?.abort();
    this.save();
    return job;
  }

  async pump() {
    if (this.pumping) return;
    this.pumping = true;
    try {
      while (true) {
        const job = this.jobs.find((item) => item.status === 'queued');
        if (!job) break;
        const controller = new AbortController();
        this.controllers.set(job.id, controller);
        job.status = 'running';
        job.updatedAt = now();
        this.save();
        try {
          const configured = this.projects.find((item) => item.id === job.projectId);
          const project = this.workspaceRoot
            ? { ...configured, path: path.join(this.workspaceRoot, job.vibeSessionId) }
            : configured;
          const outcome = await this.run(job, project, controller.signal, (text) => {
            if (job.status === 'running' && this.agent(job.provider)?.capabilities.progress) {
              const progress = String(text || '').replace(/[\x00-\x1f\x7f]+/g, ' ').trim().slice(0, 80);
              if (!progress || progress === job.progress) return;
              job.progress = progress;
              job.progressAt = now();
              job.updatedAt = job.progressAt;
              const current = Date.now();
              if (current - (this.progressSavedAt.get(job.id) || 0) >= 750) {
                this.progressSavedAt.set(job.id, current);
                this.save();
              }
            }
          }, (sessionId) => {
            const plugin = this.agent(job.provider);
            if (job.status === 'running' && plugin?.capabilities.session === 'native' &&
                validAgentSessionId(plugin, sessionId) &&
                (!job.sessionId || job.sessionId === sessionId)) {
              job.sessionId = sessionId;
              this.sessions.setCliSession(job.vibeSessionId, sessionId);
              job.updatedAt = now();
              this.save();
            }
          });
          if (job.status === 'running') {
            if (!outcome || !['completed', 'handed_off'].includes(outcome.status || 'completed')) {
              throw new Error('编程工具插件返回了无效的任务状态');
            }
            job.status = outcome.status || 'completed';
            job.result = outcome.result || job.result;
            if (outcome.handoff && typeof outcome.handoff === 'object') job.handoff = { ...outcome.handoff };
            if (job.status === 'completed' && typeof outcome.gitSnapshot === 'string') {
              this.sessions.setGitSnapshot(job.vibeSessionId, outcome.gitSnapshot);
            }
            job.updatedAt = now();
            this.save();
          }
        } catch (error) {
          if (job.status === 'running') {
            const retryableConflict = error?.retryable === true;
            job.status = retryableConflict ? 'waiting_confirmation' : 'failed';
            job.retryPending = retryableConflict;
            job.error = error instanceof Error ? error.message : String(error);
            if (retryableConflict) {
              job.progress = '';
              job.progressAt = '';
            }
            job.updatedAt = now();
            this.save();
          }
        } finally {
          this.controllers.delete(job.id);
          this.progressSavedAt.delete(job.id);
        }
      }
    } finally {
      this.pumping = false;
    }
  }
}
