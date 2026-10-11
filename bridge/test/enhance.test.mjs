import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { JobStore } from '../src/jobs.mjs';
import { createBoardLink } from '../src/board-link.mjs';
import { createPluginRegistry } from '../src/plugins/registry.mjs';
import { createEnhancement } from '../src/plugins/enhance.mjs';

const catalog = {
  events: [{ name: 'boot.click', cadence: 'edge' }],
  commands: [{ name: 'caption.show' }, { name: 'audio.play' }],
};

function boardPlugin(extra = {}) {
  return {
    apiVersion: 1,
    kind: 'board-plugin',
    id: 'board-' + Math.random().toString(36).slice(2, 8),
    label: '测试管控',
    events: ['boot.click'],
    commands: ['caption.show'],
    probe: () => ({ available: true }),
    onEvent: () => ({}),
    ...extra,
  };
}

function agentPlugin() {
  return {
    apiVersion: 1,
    kind: 'coding-agent',
    id: 'fake-agent',
    label: '假助手',
    capabilities: { session: 'native', model: false, cancel: true, progress: true },
    probe: () => ({ available: true }),
    run: async () => ({ status: 'completed', result: 'fake done' }),
  };
}

function makeSetup(plugins, run) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-enhance-'));
  // 会话与任务都要校验助手存在，默认注入假助手；用例自带同 ID 时不再重复。
  const all = plugins.some((plugin) => plugin.id === 'fake-agent') ? plugins : [agentPlugin(), ...plugins];
  const registry = createPluginRegistry(all);
  const config = { pluginsRuntime: registry, pluginSettings: {}, locale: 'zh-CN' };
  const enhance = createEnhancement(config);
  const store = new JobStore({
    file: path.join(directory, 'jobs.json'),
    projects: [{ id: 'demo', path: directory }],
    defaultProject: 'demo',
    defaultProvider: 'fake-agent',
    pluginsRuntime: registry,
    hooks: enhance,
    run,
    providerAvailable: () => ({ available: true }),
  });
  const board = createBoardLink();
  enhance.attach({ jobs: store, board, themes: () => [] });
  return { directory, config, enhance, store, board, registry };
}

function makeSession(store, provider = 'fake-agent') {
  return store.createSession({ provider, projectId: 'demo' });
}

function settle() {
  return new Promise((resolve) => setTimeout(resolve, 50));
}

test('增强钩子声明了就必须是函数', () => {
  assert.throws(() => createPluginRegistry([boardPlugin({ onTurnInput: 1 })]), /onTurnInput 必须是函数/);
  assert.throws(() => createPluginRegistry([boardPlugin({ onJob: 'x' })]), /onJob 必须是函数/);
  const ok = createPluginRegistry([boardPlugin({ onTurnOutput: () => 'x' })]);
  assert.equal(ok.boardPlugins().length, 1);
});

test('回合输入管道：链式改写实际执行的指令，存储原文不变', async () => {
  const seen = [];
  const { directory, store } = makeSetup([
    boardPlugin({ onTurnInput: ({ instruction }) => `${instruction}-A` }),
    boardPlugin({ onTurnInput: ({ instruction }) => `${instruction}-B` }),
  ], async (job) => {
    seen.push(job.instruction);
    return { status: 'completed', result: 'ok' };
  });
  const session = makeSession(store);
  const job = store.submit({ instruction: '原始指令', sessionId: session.id, provider: 'fake-agent', projectId: 'demo' });
  store.confirm(job.id);
  await settle();
  assert.deepEqual(seen, ['原始指令-A-B']);
  assert.equal(store.get(job.id).instruction, '原始指令');
  fs.rmSync(directory, { recursive: true, force: true });
});

test('回合输出管道：仅完成时改写结果', async () => {
  const { directory, store } = makeSetup([
    boardPlugin({ onTurnOutput: ({ result }) => result.toUpperCase() }),
  ], async () => ({ status: 'completed', result: 'done text' }));
  const session = makeSession(store);
  const job = store.submit({ instruction: '跑一下', sessionId: session.id, provider: 'fake-agent', projectId: 'demo' });
  store.confirm(job.id);
  await settle();
  assert.equal(store.get(job.id).result, 'DONE TEXT');
  fs.rmSync(directory, { recursive: true, force: true });
});

test('钩子抛错或返回非字符串时沿用上一环', async () => {
  const seen = [];
  const { directory, store } = makeSetup([
    boardPlugin({ onTurnInput: () => { throw new Error('炸了'); } }),
    boardPlugin({ onTurnInput: () => 42 }),
    boardPlugin({ onTurnInput: ({ instruction }) => `${instruction}!` }),
  ], async (job) => {
    seen.push(job.instruction);
    return { status: 'completed', result: 'ok' };
  });
  const session = makeSession(store);
  const job = store.submit({ instruction: '原文指令', sessionId: session.id, provider: 'fake-agent', projectId: 'demo' });
  store.confirm(job.id);
  await settle();
  assert.deepEqual(seen, ['原文指令!']);
  assert.equal(store.get(job.id).result, 'ok');
  fs.rmSync(directory, { recursive: true, force: true });
});

test('生命周期通知按序到达；插件创建的任务不广播 created', async () => {
  const events = [];
  const { directory, store, enhance } = makeSetup([
    boardPlugin({ onJob: ({ type, job }) => events.push(`${type}:${job.source}:${job.id}`) }),
  ], async () => ({ status: 'completed', result: 'ok' }));
  const session = makeSession(store);
  const job = store.submit({ instruction: '语音任务', sessionId: session.id, provider: 'fake-agent', projectId: 'demo', source: 'voice' });
  store.confirm(job.id);
  await settle();
  assert.deepEqual(events.map((item) => item.split(':')[0]), ['created', 'confirmed', 'started', 'completed']);

  events.length = 0;
  const created = enhance.bridge().createTask({ instruction: '插件任务' });
  assert.equal(created.job.status, 'waiting_confirmation');
  assert.equal(created.job.source, 'plugin');
  await settle();
  assert.equal(events.filter((item) => item.startsWith('created')).length, 0, '插件任务不广播 created');
  enhance.bridge().confirmJob(created.job.id);
  await settle();
  assert.ok(events.includes(`confirmed:plugin:${created.job.id}`));
  assert.equal(store.get(created.job.id).status, 'completed');
  fs.rmSync(directory, { recursive: true, force: true });
});

test('bridge 句柄：助手清单、直接入队命令、事件上下文携带 bridge', async () => {
  const contexts = [];
  const { directory, config, board, registry, enhance } = makeSetup([
    boardPlugin({ onEvent: (ctx) => { contexts.push(ctx); return {}; } }),
  ], async () => ({ status: 'completed', result: 'ok' }));
  const bridge = enhance.bridge();
  assert.deepEqual(bridge.agents().map((agent) => agent.id), ['fake-agent']);
  assert.equal(bridge.agent('fake-agent').available, true);

  board.setCatalog('dev1', catalog);
  assert.deepEqual(bridge.devices(), ['dev1']);
  const queued = bridge.send('dev1', { name: 'caption.show', fields: { text: '你好' } });
  assert.equal(queued.name, 'caption.show');
  assert.equal(board.pending('dev1').length, 1);

  await board.handleEvent('dev1', { name: 'boot.click', fields: {} }, registry, config, bridge);
  assert.equal(contexts.length, 1);
  assert.equal(typeof contexts[0].bridge.createTask, 'function');
  fs.rmSync(directory, { recursive: true, force: true });
});

test('createTask 限流：超出每分钟上限后拒绝', async () => {
  const { directory, enhance } = makeSetup([], async () => ({ status: 'completed', result: 'ok' }));
  const bridge = enhance.bridge();
  for (let index = 0; index < 10; index += 1) bridge.createTask({ instruction: `第 ${index} 个任务` });
  assert.throws(() => bridge.createTask({ instruction: '超额任务' }), /过于频繁/);
  fs.rmSync(directory, { recursive: true, force: true });
});
