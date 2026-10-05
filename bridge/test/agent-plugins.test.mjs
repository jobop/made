import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createRunner, providerStates } from '../src/providers.mjs';
import { JobStore } from '../src/jobs.mjs';

async function until(check) {
  const deadline = Date.now() + 1500;
  while (!check()) {
    if (Date.now() > deadline) throw new Error('等待插件测试超时');
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
}

function fixture(t, plugins, models = {}) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-agent-plugins-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const pluginsRuntime = {
    agents: () => plugins,
    agent: (id) => plugins.find((plugin) => plugin.id === id),
    hasAgent: (id) => plugins.some((plugin) => plugin.id === id),
  };
  const config = { pluginsRuntime, models };
  const file = path.join(directory, 'jobs.json');
  const options = {
    file, projects: [{ id: 'demo', path: directory }], defaultProject: 'demo',
    defaultProvider: plugins[0].id, pluginsRuntime,
    run: createRunner(config),
    providerAvailable: (id) => providerStates(config).find((state) => state.id === id),
  };
  return { config, file, options, store: new JobStore(options) };
}

const manifest = {
  apiVersion: 1, kind: 'coding-agent', id: 'local-test-agent', label: '本地测试助手', icon: 'owl',
  capabilities: { session: 'native', model: true, cancel: true, progress: true },
  model: { default: 'fake-small', required: false },
  probe: () => ({ available: true, mode: 'local_test', reason: '' }),
};

test('外部助手无需内建分支即可跨重启续聊、选择模型、回显进度和取消', async (t) => {
  const seen = [];
  let abortSeen = false;
  const plugin = {
    ...manifest,
    async run({ job, model, signal, onProgress, onSession }) {
      seen.push({ model, sessionId: job.sessionId });
      onSession('external-session:001');
      onProgress('正在检查测试项目');
      if (seen.length === 3) {
        await new Promise((resolve) => signal.addEventListener('abort', () => { abortSeen = true; resolve(); }, { once: true }));
      }
      return { status: 'completed', result: `第 ${seen.length} 次回复` };
    },
  };
  const { store, config, options } = fixture(t, [plugin], { [plugin.id]: 'fake-small' });
  assert.equal(providerStates(config)[0].label, '本地测试助手');
  assert.equal(providerStates(config)[0].capabilities.session, 'native');
  const session = store.createSession({ provider: plugin.id });
  const first = store.submit({ sessionId: session.id, instruction: '记住这个测试上下文' });
  assert.equal(seen.length, 0);
  store.confirm(first.id);
  await until(() => first.status === 'completed');
  assert.equal(first.progress, '正在检查测试项目');
  assert.equal(store.sessions.get(session.id).cliSessionId, 'external-session:001');
  assert.equal(store.sessions.get(session.id).title, '记住这个测试上下文');

  const restored = new JobStore(options);
  config.models[plugin.id] = 'fake-large';
  const second = restored.submit({ sessionId: session.id, instruction: '继续上面的工作' });
  restored.confirm(second.id);
  await until(() => second.status === 'completed');
  assert.deepEqual(seen, [
    { model: 'fake-small', sessionId: undefined },
    { model: 'fake-large', sessionId: 'external-session:001' },
  ]);
  const third = restored.submit({ sessionId: session.id, instruction: '测试取消进行中的工作' });
  restored.confirm(third.id);
  restored.cancel(third.id);
  await until(() => abortSeen && !restored.pumping);
  assert.equal(third.status, 'cancelled');
  assert.equal(third.result, '');
  assert.equal(restored.listSessions().length, 1);
});

test('插件能力控制无模型、无进度、无独立上下文和运行中不可撤回的工具', async (t) => {
  let release;
  let passedModel;
  const plugin = {
    ...manifest, id: 'external-dispatch',
    capabilities: { session: 'external_unscoped', model: false, cancel: false, progress: false },
    async run({ model, onProgress, onSession }) {
      passedModel = model;
      onProgress('这个进度不应显示');
      onSession('ignored-session');
      await new Promise((resolve) => { release = resolve; });
      return { status: 'handed_off', result: '已转交', handoff: { cursor: 'reply-1', readState: 'retrying' } };
    },
    async readUpdates({ job }) {
      return {
        handoff: { ...job.handoff, cursor: 'reply-2', readState: 'readable', replyCount: 1 },
        appendResult: '外部工具的回复',
      };
    },
  };
  const { store, file } = fixture(t, [plugin], { [plugin.id]: 'must-not-pass' });
  const session = store.createSession({});
  assert.equal(session.contextMode, 'external_unscoped');
  const job = store.submit({ sessionId: session.id, instruction: '发送这个测试消息' });
  store.confirm(job.id);
  assert.equal(passedModel, undefined);
  assert.equal(job.progress, '');
  assert.equal(job.sessionId, undefined);
  assert.throws(() => store.cancel(job.id), /不支持撤回/);
  release();
  await until(() => job.status === 'handed_off');
  await store.refreshHandoffs({ force: true });
  assert.equal(job.handoff.cursor, 'reply-2');
  assert.match(job.result, /外部工具的回复/);
  assert.equal(JSON.parse(fs.readFileSync(file, 'utf8'))[0].handoff.replyCount, 1);
  const next = store.submit({ sessionId: session.id, instruction: '继续这个外部任务' });
  store.cancel(next.id);
  assert.equal(next.status, 'cancelled');
});

test('删除插件后保留历史记录，并拒绝启动或新建对应助手任务', async (t) => {
  const plugins = [{ ...manifest, run: async () => ({ status: 'completed', result: '完成' }) }];
  const { store, options } = fixture(t, plugins);
  const session = store.createSession({});
  const job = store.submit({ sessionId: session.id, instruction: '等待确认的任务' });
  plugins.splice(0);
  const restored = new JobStore(options);
  assert.equal(restored.listSessions()[0].id, session.id);
  assert.equal(restored.get(job.id).instruction, '等待确认的任务');
  assert.throws(() => restored.confirm(job.id), /尚未就绪/);
  assert.throws(() => restored.createSession({}), /不支持/);
  assert.throws(() => restored.submit({ sessionId: session.id, instruction: '不能启动的任务' }), /不支持/);
});
