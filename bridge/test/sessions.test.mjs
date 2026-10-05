import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { JobStore } from '../src/jobs.mjs';
import { createRunner } from '../src/providers.mjs';

const CLI_ID = '0199a213-81c0-7800-8aa1-bbab2a035a53';

async function until(check) {
  const deadline = Date.now() + 3000;
  while (!check()) {
    if (Date.now() > deadline) throw new Error('超时');
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
}

function makeStore(directory, run = async () => ({ status: 'completed', result: '完成' })) {
  return new JobStore({
    file: path.join(directory, 'jobs.json'),
    projects: [{ id: 'demo', path: directory }],
    defaultProject: 'demo', defaultProvider: 'codex', run,
    providerAvailable: () => ({ available: true }),
  });
}

test('会话显式创建、首轮自动命名、标题可修改；轮次串行并跨重启保留上下文 ID', async (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-sessions-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  let release;
  const seen = [];
  const store = makeStore(directory, async (job, _project, _signal, _progress, onSession) => {
    seen.push(job.sessionId);
    if (seen.length === 1) {
      onSession(CLI_ID);
      await new Promise((resolve) => { release = resolve; });
    }
    return { status: 'completed', result: '完成' };
  });
  assert.throws(() => store.submit({ instruction: '检查页面标题' }), /新建任务/);
  const session = store.createSession({ provider: 'codex', projectId: 'demo' });
  assert.match(session.id, /^[0-9a-f-]{36}$/);
  assert.equal(session.title, '新任务');
  assert.equal(session.status, 'empty');
  const first = store.submit({ instruction: '检查页面标题😔', sessionId: session.id });
  assert.equal(first.vibeSessionId, session.id);
  assert.equal(store.listSessions()[0].title, '检查页面标题😔');
  assert.throws(() => store.submit({ instruction: '继续修改标题', sessionId: session.id }), /上一条/);
  assert.throws(() => store.submit({ instruction: '继续修改标题', sessionId: session.id, provider: 'cursor' }), /不匹配/);
  store.confirm(first.id);
  await until(() => first.status === 'running');
  assert.throws(() => store.submit({ instruction: '继续修改标题', sessionId: session.id }), /上一条/);
  release();
  await until(() => first.status === 'completed');
  const second = store.submit({ instruction: '继续修改标题', sessionId: session.id });
  assert.equal(second.sessionId, CLI_ID);
  store.confirm(second.id);
  await until(() => second.status === 'completed');
  assert.deepEqual(seen, [undefined, CLI_ID]);
  assert.equal(store.renameSession(session.id, '首页优化').title, '首页优化');
  const restored = makeStore(directory);
  assert.equal(restored.listSessions()[0].title, '首页优化');
  assert.equal(restored.listSessions()[0].cliSessionId, CLI_ID);
  assert.deepEqual(restored.list().map((job) => job.vibeSessionId), [session.id, session.id]);
});

test('旧任务一条对应一个历史会话，保留 CLI 对话号和原任务', (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-migrate-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  fs.writeFileSync(path.join(directory, 'jobs.json'), JSON.stringify([
    { id: 'old-1', instruction: '修改首页', provider: 'codex', projectId: 'demo', status: 'completed', sessionId: CLI_ID, createdAt: '2026-01-01T00:00:00.000Z', updatedAt: '2026-01-01T00:00:00.000Z' },
    { id: 'old-2', instruction: '修复按钮', provider: 'cursor', projectId: 'demo', status: 'completed', createdAt: '2026-01-02T00:00:00.000Z', updatedAt: '2026-01-02T00:00:00.000Z' },
  ]));
  const store = makeStore(directory);
  const sessions = store.listSessions();
  assert.equal(sessions.length, 2);
  assert.notEqual(store.get('old-1').vibeSessionId, store.get('old-2').vibeSessionId);
  assert.equal(store.sessions.get(store.get('old-1').vibeSessionId).cliSessionId, CLI_ID);
  assert.equal(store.sessions.get(store.get('old-2').vibeSessionId).title, '修复按钮');
  assert.ok(sessions.every((session) => session.legacy));
  assert.equal(makeStore(directory).listSessions().length, 2);
});

test('删除会话同时清理历史与待确认轮次，重启后不会重新生成', (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-delete-session-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const store = makeStore(directory);
  const removed = store.createSession();
  const retained = store.createSession();
  const waiting = store.submit({ instruction: '待确认的旧指令', sessionId: removed.id });
  const other = store.submit({ instruction: '另一任务的指令', sessionId: retained.id });
  assert.deepEqual(store.deleteSession(removed.id), { id: removed.id, deleted: true });
  assert.equal(waiting.status, 'deleted');
  assert.equal(store.get(waiting.id), undefined);
  assert.equal(store.get(other.id)?.id, other.id);
  assert.deepEqual(store.listSessions().map((session) => session.id), [retained.id]);
  assert.throws(() => store.submit({ instruction: '继续旧任务', sessionId: removed.id }), /任务不存在/);
  assert.throws(() => store.deleteSession(removed.id), /任务不存在/);
  const restored = makeStore(directory);
  assert.deepEqual(restored.listSessions().map((session) => session.id), [retained.id]);
  assert.deepEqual(restored.list().map((job) => job.id), [other.id]);
});

test('删除正在运行的会话会发出取消信号；不支持取消的插件会拒绝删除', async (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-delete-running-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  let release;
  let aborted = false;
  const store = makeStore(directory, async (_job, _project, signal) => {
    await new Promise((resolve) => {
      release = resolve;
      signal.addEventListener('abort', () => { aborted = true; resolve(); }, { once: true });
    });
    return { status: 'completed', result: '晚到的执行结果' };
  });
  const session = store.createSession();
  const job = store.submit({ instruction: '执行中的指令', sessionId: session.id });
  store.confirm(job.id);
  await until(() => job.status === 'running');
  assert.deepEqual(store.deleteSession(session.id), { id: session.id, deleted: true });
  assert.equal(aborted, true);
  await until(() => !store.pumping);
  assert.equal(store.get(job.id), undefined);
  assert.equal(store.sessions.get(session.id), undefined);
  assert.equal(makeStore(directory).list().length, 0);
  release?.();

  const external = store.createSession({ provider: 'workbuddy' });
  const externalJob = store.submit({ instruction: 'WorkBuddy 正在执行', sessionId: external.id });
  externalJob.status = 'running';
  assert.throws(() => store.deleteSession(external.id), /不支持撤回/);
  assert.equal(store.sessions.get(external.id)?.id, external.id);
  assert.equal(store.get(externalJob.id)?.id, externalJob.id);
});

test('原生续接缺失会话号时拒绝假续聊，WorkBuddy 明示外部上下文未隔离', async (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-context-mode-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const store = makeStore(directory);
  const codex = store.createSession({ provider: 'codex' });
  const first = store.submit({ instruction: '检查仓库', sessionId: codex.id });
  store.confirm(first.id);
  await until(() => first.status === 'completed');
  assert.throws(() => store.submit({ instruction: '继续检查', sessionId: codex.id }), /未返回可恢复的会话 ID/);
  const workbuddy = store.createSession({ provider: 'workbuddy' });
  assert.equal(workbuddy.contextMode, 'external_unscoped');
  const external = store.submit({ instruction: '检查文案', sessionId: workbuddy.id });
  store.confirm(external.id);
  await until(() => external.status === 'completed');
  assert.equal(store.submit({ instruction: '继续检查文案', sessionId: workbuddy.id }).vibeSessionId, workbuddy.id);
});

test('Codex、Cursor、Qoder 后续轮次使用各自 CLI 的恢复参数', async (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-cli-resume-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const bin = path.join(directory, 'bin');
  fs.mkdirSync(bin);
  const log = path.join(directory, 'args.jsonl');
  const script = `#!/usr/bin/env node
const fs = require('node:fs');
const path = require('node:path');
const command = path.basename(process.argv[1]);
fs.appendFileSync(${JSON.stringify(log)}, JSON.stringify({ command, args: process.argv.slice(2) }) + '\\n');
if (command === 'codex' && !process.argv.includes('resume')) {
  fs.writeFileSync(path.join(process.cwd(), 'agent-change.txt'), '第一轮修改');
}
const id = ${JSON.stringify(CLI_ID)};
const events = command === 'codex'
  ? [{ type: 'thread.started', thread_id: id }, { type: 'item.completed', item: { type: 'agent_message', text: '完成' } }]
  : command === 'cursor-agent'
    ? [{ type: 'result', subtype: 'success', is_error: false, result: '完成', session_id: id }]
    : [{ type: 'system', subtype: 'init', data: { session_id: id } }, { type: 'result', subtype: 'success', is_error: false, result: '完成' }];
for (const event of events) process.stdout.write(JSON.stringify(event) + '\\n');
`;
  for (const command of ['codex', 'cursor-agent', 'qoder']) {
    fs.writeFileSync(path.join(bin, command), script, { mode: 0o755 });
  }
  const previousPath = process.env.PATH;
  process.env.PATH = `${bin}${path.delimiter}${previousPath || ''}`;
  t.after(() => { process.env.PATH = previousPath; });
  const config = { models: { codex: 'gpt-6-astra', cursor: '', qoder: '' } };
  for (const [provider, command] of [['codex', 'codex'], ['cursor', 'cursor-agent'], ['qoder', 'qoder']]) {
    const repoPath = path.join(directory, provider);
    fs.mkdirSync(repoPath);
    const repo = fs.realpathSync(repoPath);
    assert.equal(spawnSync('git', ['init', '-q', repo]).status, 0);
    const userFile = path.join(repo, 'human-note.txt');
    fs.writeFileSync(userFile, '新任务开始前已有未提交改动');
    const store = new JobStore({
      file: path.join(directory, `${provider}-jobs.json`),
      projects: [{ id: 'demo', path: repo }],
      defaultProject: 'demo', defaultProvider: provider,
      run: createRunner(config), providerAvailable: () => ({ available: true }),
    });
    const session = store.createSession({ provider });
    for (const instruction of ['检查代码', '继续检查']) {
      const job = store.submit({ instruction, sessionId: session.id });
      store.confirm(job.id);
      await until(() => ['completed', 'failed'].includes(job.status));
      assert.equal(job.status, 'completed', `${provider}: ${job.error}`);
      if (provider === 'codex' && instruction === '检查代码') {
        // A user may review and commit the first turn before continuing.
        assert.equal(spawnSync('git', ['add', 'agent-change.txt'], { cwd: repo }).status, 0);
        assert.equal(spawnSync('git', ['-c', 'user.name=Test', '-c', 'user.email=test@example.com',
          'commit', '-qm', 'Review first turn'], { cwd: repo }).status, 0);
      }
      if (instruction === '检查代码') {
        fs.writeFileSync(userFile, '两轮之间用户又修改了文件');
      }
    }
    assert.equal(store.sessions.get(session.id).cliSessionId, CLI_ID);
    const calls = fs.readFileSync(log, 'utf8').trim().split('\n').map(JSON.parse)
      .filter((entry) => entry.command === command);
    assert.equal(calls.length, 2);
    const second = calls[1].args;
    if (provider === 'codex') {
      assert.deepEqual(second.slice(0, 8), ['exec', '--json', '--sandbox', 'workspace-write', '-C', repo, 'resume', '-m']);
      assert.ok(second.includes(CLI_ID));
    } else {
      const resumeFlag = provider === 'cursor' ? '--resume' : '--session-id';
      assert.equal(second[second.indexOf(resumeFlag) + 1], CLI_ID);
    }
  }
});

test('Codex 对话被占用时保留同一轮待确认，安全提示且只在再次确认后重试', async (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-active-writer-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const repoPath = path.join(directory, 'repo');
  fs.mkdirSync(repoPath);
  const repo = fs.realpathSync(repoPath);
  assert.equal(spawnSync('git', ['init', '-q', repo]).status, 0);
  const bin = path.join(directory, 'bin');
  fs.mkdirSync(bin);
  const calls = path.join(directory, 'calls.txt');
  const conflictSeen = path.join(directory, 'conflict-seen');
  const script = `#!/usr/bin/env node
const fs = require('node:fs');
const args = process.argv.slice(2);
fs.appendFileSync(${JSON.stringify(calls)}, (args.includes('resume') ? 'resume' : 'start') + '\\n');
const id = ${JSON.stringify(CLI_ID)};
if (args.includes('resume') && !fs.existsSync(${JSON.stringify(conflictSeen)})) {
  fs.writeFileSync(${JSON.stringify(conflictSeen)}, '1');
  process.stderr.write('Failed to create session: thread-store conflict: thread ' + id + ' already has an active writer\\n');
  process.stderr.write('at private/internal/stack:42 SECRET\\n');
  process.exit(1);
}
process.stdout.write(JSON.stringify({ type: 'thread.started', thread_id: id }) + '\\n');
process.stdout.write(JSON.stringify({ type: 'item.completed', item: { type: 'agent_message', text: '完成' } }) + '\\n');
`;
  fs.writeFileSync(path.join(bin, 'codex'), script, { mode: 0o755 });
  const previousPath = process.env.PATH;
  process.env.PATH = `${bin}${path.delimiter}${previousPath || ''}`;
  t.after(() => { process.env.PATH = previousPath; });
  const file = path.join(directory, 'jobs.json');
  const store = new JobStore({ file, projects: [{ id: 'demo', path: repo }],
    defaultProject: 'demo', defaultProvider: 'codex',
    run: createRunner({ models: { codex: 'gpt-6-astra' } }),
    providerAvailable: () => ({ available: true }) });
  const session = store.createSession({ provider: 'codex' });
  const first = store.submit({ instruction: '检查代码', sessionId: session.id });
  store.confirm(first.id);
  await until(() => first.status === 'completed');
  const followup = store.submit({ instruction: '继续检查代码', sessionId: session.id });
  store.confirm(followup.id);
  await until(() => followup.retryPending === true);
  assert.equal(followup.status, 'waiting_confirmation');
  assert.equal(followup.error, '此 Codex 对话正在另一窗口或进程使用；结束后再次确认即可继续');
  assert.doesNotMatch(JSON.stringify(followup), /thread-store conflict|private\/internal|SECRET/);
  assert.equal(followup.sessionId, CLI_ID);
  assert.equal(store.sessions.get(session.id).cliSessionId, CLI_ID);
  assert.equal(store.list().length, 2);
  assert.equal(JSON.parse(fs.readFileSync(file, 'utf8'))[1].retryPending, true);
  await new Promise((resolve) => setTimeout(resolve, 100));
  assert.deepEqual(fs.readFileSync(calls, 'utf8').trim().split('\n'), ['start', 'resume']);
  store.confirm(followup.id);
  assert.equal(followup.retryPending, undefined);
  assert.equal(followup.error, '');
  await until(() => followup.status === 'completed');
  assert.deepEqual(fs.readFileSync(calls, 'utf8').trim().split('\n'), ['start', 'resume', 'resume']);
  assert.equal(store.list().length, 2);
});
