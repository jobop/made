import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { JobStore } from '../src/jobs.mjs';
import { createRunner } from '../src/providers.mjs';
import { loadConfig } from '../src/config.mjs';
import { transcribe, validateVoiceWav } from '../src/transcribe.mjs';

function sampleWav() {
  const wav = Buffer.alloc(44 + 16000);
  wav.write('RIFF', 0);
  wav.writeUInt32LE(wav.length - 8, 4);
  wav.write('WAVEfmt ', 8);
  wav.writeUInt32LE(16, 16);
  wav.writeUInt16LE(1, 20);
  wav.writeUInt16LE(1, 22);
  wav.writeUInt32LE(16000, 24);
  wav.writeUInt32LE(32000, 28);
  wav.writeUInt16LE(2, 32);
  wav.writeUInt16LE(16, 34);
  wav.write('data', 36);
  wav.writeUInt32LE(16000, 40);
  return wav;
}

function fixture(run = async () => ({ status: 'completed', result: '完成' })) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-bridge-'));
  const projects = [{ id: 'demo', label: '演示项目', path: directory }];
  const config = { projects, defaultProject: 'demo', defaultProvider: 'codex' };
  const store = new JobStore({
    file: path.join(directory, 'jobs.json'),
    projects,
    defaultProject: 'demo',
    defaultProvider: 'codex',
    run,
    providerAvailable: () => ({ available: true }),
  });
  return { store, config, directory };
}

function submit(store, payload) {
  const session = store.createSession({ provider: payload.provider });
  return store.submit({ ...payload, sessionId: session.id });
}

function workbuddyFixture() {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-workbuddy-'));
  const projects = [{ id: 'demo', label: '演示项目', path: directory }];
  const config = {
    projects, defaultProject: 'demo', defaultProvider: 'workbuddy',
    workbuddyToken: 'test-token', workbuddy: { allowUnscopedDispatch: true },
  };
  const file = path.join(directory, 'jobs.json');
  const store = new JobStore({
    file, projects, defaultProject: 'demo', defaultProvider: 'workbuddy',
    run: createRunner(config), providerAvailable: () => ({ available: true }),
  });
  return { store, file };
}

async function until(check) {
  const deadline = Date.now() + 1000;
  while (!check()) {
    if (Date.now() > deadline) throw new Error('超时');
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
}

function fakeCliFixture(t, command, script, runnerConfig = {}) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-cli-session-'));
  const bin = path.join(directory, 'bin');
  const repoPath = path.join(directory, 'repo');
  fs.mkdirSync(bin);
  fs.mkdirSync(repoPath);
  const repo = fs.realpathSync(repoPath);
  assert.equal(spawnSync('git', ['init', '-q', repo]).status, 0);
  fs.writeFileSync(path.join(bin, command), `#!/usr/bin/env node\n${script}`, { mode: 0o755 });
  const previousPath = process.env.PATH;
  process.env.PATH = `${bin}${path.delimiter}${previousPath || ''}`;
  t.after(() => { process.env.PATH = previousPath; });
  const file = path.join(directory, 'jobs.json');
  const projects = [{ id: 'demo', label: '演示项目', path: repo }];
  const store = new JobStore({
    file, projects, defaultProject: 'demo', defaultProvider: command === 'codex' ? 'codex' : 'cursor',
    run: createRunner(runnerConfig), providerAvailable: () => ({ available: true }),
  });
  return { store, file, projects };
}

test('Codex 使用项目默认模型，环境变量可以覆盖，旧配置也有可用默认值', async (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-codex-model-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const configPath = path.join(directory, 'config.json');
  const raw = { projects: [{ id: 'demo', path: directory }] };
  fs.writeFileSync(configPath, JSON.stringify(raw));
  const env = { VIBE_CONFIG: configPath, VIBE_STATE_DIR: directory };
  assert.equal(loadConfig(env).codexModel, 'gpt-6-astra');
  assert.equal(JSON.parse(fs.readFileSync(new URL('../config.example.json', import.meta.url))).codexModel, 'gpt-6-astra');
  assert.equal(loadConfig({ ...env, VIBE_CODEX_MODEL: 'gpt-5.5' }).codexModel, 'gpt-5.5');
  const script = `
const index = process.argv.indexOf('-m');
if (index < 0 || process.argv[index + 1] !== 'gpt-6-astra') process.exit(7);
process.stdout.write(JSON.stringify({ type: 'item.completed', item: { type: 'agent_message', text: '模型正确' } }) + '\\n');
`;
  const { store } = fakeCliFixture(t, 'codex', script, loadConfig(env));
  const job = submit(store, { instruction: '检查演示项目的代码' });
  store.confirm(job.id);
  await until(() => job.status === 'completed' || job.status === 'failed');
  assert.equal(job.status, 'completed', job.error);
  assert.match(job.result, /模型正确/);
});

test('任务只在确认后运行，状态会保存', async () => {
  let calls = 0;
  const { store, directory } = fixture(async () => {
    calls += 1;
    return { status: 'completed', result: '已写入代码' };
  });
  const job = submit(store, { instruction: '修复演示项目的登录页' });
  assert.equal(job.status, 'waiting_confirmation');
  assert.equal(calls, 0);
  store.confirm(job.id);
  await until(() => job.status === 'completed');
  assert.equal(calls, 1);
  assert.equal(JSON.parse(fs.readFileSync(path.join(directory, 'jobs.json')))[0].status, 'completed');
});

test('确认前取消不会运行；运行中取消会传递中断信号', async () => {
  let aborted = false;
  const { store } = fixture(async (_job, _project, signal) => {
    await new Promise((resolve) => {
      signal.addEventListener('abort', () => {
        aborted = true;
        resolve();
      }, { once: true });
    });
    return { status: 'completed', result: '不应覆盖取消状态' };
  });
  const first = submit(store, { instruction: '先不要执行这个任务' });
  store.cancel(first.id);
  assert.equal(first.status, 'cancelled');
  assert.throws(() => store.confirm(first.id));
  const second = submit(store, { instruction: '执行后马上取消这个任务' });
  store.confirm(second.id);
  await until(() => second.status === 'running');
  store.cancel(second.id);
  await until(() => aborted);
  assert.equal(second.status, 'cancelled');
});

test('Codex 保存可继续使用的线程 ID，拒绝伪造 ID，并且不使用临时会话模式', async (t) => {
  const sessionId = '0199a213-81c0-7800-8aa1-bbab2a035a53';
  const fakeEvents = [
    { type: 'thread.started', thread_id: '../invalid' },
    { type: 'thread.started', thread_id: sessionId },
    { type: 'item.completed', item: { type: 'agent_message', text: '检查完成' } },
  ].map((event) => `${JSON.stringify(event)}\n`).join('');
  const script = `
if (process.argv.includes('--ephemeral')) process.exit(7);
const output = ${JSON.stringify(fakeEvents)};
process.stdout.write(output.slice(0, 19));
setTimeout(() => process.stdout.write(output.slice(19)), 20);
`;
  const { store, file, projects } = fakeCliFixture(t, 'codex', script);
  const job = submit(store, { instruction: '检查演示项目的代码' });
  store.confirm(job.id);
  await until(() => job.status === 'completed');
  assert.equal(job.sessionId, sessionId);
  assert.match(job.result, /检查完成/);
  assert.equal(JSON.parse(fs.readFileSync(file, 'utf8'))[0].sessionId, sessionId);
  const reloaded = new JobStore({
    file, projects, defaultProject: 'demo', defaultProvider: 'codex',
    run: async () => {}, providerAvailable: () => ({ available: true }),
  });
  assert.equal(reloaded.get(job.id).sessionId, sessionId);
});

test('Cursor 从成功结果中保存会话 ID，不接受结果正文里的伪造字段', async (t) => {
  const sessionId = 'c6b62c6f-7ead-4fd6-9922-e952131177ff';
  const response = {
    type: 'result', subtype: 'success', is_error: false,
    result: '内容里有 session_id=00000000-0000-0000-0000-000000000000，但以结构化字段为准',
    session_id: sessionId,
  };
  const { store, file } = fakeCliFixture(t, 'cursor-agent', `process.stdout.write(${JSON.stringify(JSON.stringify(response) + '\n')});`);
  const job = submit(store, { instruction: '检查演示项目的代码', provider: 'cursor' });
  store.confirm(job.id);
  await until(() => job.status === 'completed');
  assert.equal(job.sessionId, sessionId);
  assert.equal(JSON.parse(fs.readFileSync(file, 'utf8'))[0].sessionId, sessionId);
});

test('独立语音入口只接受有界的 16kHz 单声道 PCM WAV', () => {
  const wav = sampleWav();
  assert.equal(validateVoiceWav(wav), wav);
  const stereo = Buffer.from(wav);
  stereo.writeUInt16LE(2, 22);
  assert.throws(() => validateVoiceWav(stereo));
  const oversized = Buffer.alloc(1_100_001);
  assert.throws(() => validateVoiceWav(oversized));
});

test('取消录音会中断正在等待的语音 API 请求', async (t) => {
  const controller = new AbortController();
  let requested;
  const began = new Promise((resolve) => { requested = resolve; });
  t.mock.method(globalThis, 'fetch', async (_url, options) => {
    requested();
    return new Promise((_resolve, reject) => {
      options.signal.addEventListener('abort', () => reject(options.signal.reason), { once: true });
    });
  });
  const result = transcribe(sampleWav(), { asrMode: 'openai', openaiKey: 'test-key' }, controller.signal);
  const rejected = assert.rejects(result, { name: 'AbortError' });
  await began;
  controller.abort();
  await rejected;
});

test('OpenAI 转写只在电脑侧发送 WAV 和模型参数，返回文字待后续确认', async (t) => {
  t.mock.method(globalThis, 'fetch', async (url, options) => {
    assert.equal(url, 'https://api.openai.com/v1/audio/transcriptions');
    assert.equal(options.headers.Authorization, 'Bearer local-test-key');
    assert.equal(options.body.get('model'), 'gpt-transcribe');
    assert.equal(options.body.get('languages[]'), null);
    assert.equal(options.body.get('prompt'), null);
    assert.equal(options.body.get('language'), null);
    assert.equal(options.body.get('file').type, 'audio/wav');
    return { ok: true, json: async () => ({ text: '请用 Cursor 修改首页按钮' }) };
  });
  const result = await transcribe(sampleWav(), {
    asrMode: 'openai', openaiKey: 'local-test-key', transcribeModel: 'gpt-transcribe',
  });
  assert.equal(result, '请用 Cursor 修改首页按钮');
});

test('WorkBuddy 回读助理消息并持久化游标，不将回复当成完成，也不串到下一条用户消息', async (t) => {
  let reads = 0;
  t.mock.method(globalThis, 'fetch', async (url, options) => {
    assert.equal(options.headers.Authorization, 'Bearer test-token');
    if (options.method === 'POST') {
      assert.equal(url, 'https://www.workbuddy.cn/openapi/v2/localassistant/message');
      assert.deepEqual(JSON.parse(options.body), { content: '修改演示项目的标题', msg_type: 'text' });
      return { ok: true, status: 200, json: async () => ({ code: 0, data: { message_id: 'msg-001' } }) };
    }
    assert.equal(options.method, 'GET');
    assert.equal(new URL(url).searchParams.get('message_id'), reads === 0 ? 'msg-001' : 'msg-002');
    reads += 1;
    const messages = reads === 1
      ? [{ message_id: 'msg-002', role: 'assistant', content: ['我先检查标题。'], msg_type: 'text' }]
      : [
        { message_id: 'msg-003', role: 'user', content: ['另一个问题'], msg_type: 'text' },
        { message_id: 'msg-004', role: 'assistant', content: ['这是另一个问题的回答'], msg_type: 'text' },
      ];
    return { ok: true, status: 200, json: async () => ({ code: 0, data: { messages } }) };
  });
  const { store, file } = workbuddyFixture();
  const job = submit(store, { instruction: '修改演示项目的标题' });
  store.confirm(job.id);
  await until(() => job.status === 'handed_off');
  assert.throws(() => store.cancel(job.id), /不能取消/);
  store.list(); // The desktop and device endpoints both use this list.
  await until(() => job.workbuddyCursor === 'msg-002');
  assert.equal(job.status, 'handed_off');
  assert.match(job.result, /我先检查标题/);
  assert.equal(job.workbuddyCursor, 'msg-002');
  assert.equal(JSON.parse(fs.readFileSync(file, 'utf8'))[0].workbuddyReplyCount, 1);
  await store.refreshHandoffs({ force: true });
  assert.equal(job.workbuddyReadState, 'ambiguous');
  assert.doesNotMatch(job.result, /这是另一个问题的回答/);
  await store.refreshHandoffs({ force: true });
  assert.equal(reads, 2);
});

test('WorkBuddy 缺少消息读取授权时仍保留已转交状态和查看提示', async (t) => {
  t.mock.method(globalThis, 'fetch', async (_url, options) => {
    if (options.method === 'POST') {
      return { ok: true, status: 200, json: async () => ({ code: 0, data: { message_id: 'msg-005' } }) };
    }
    return { ok: false, status: 403 };
  });
  const { store } = workbuddyFixture();
  const job = submit(store, { instruction: '更新 README 文案' });
  store.confirm(job.id);
  await until(() => job.status === 'handed_off');
  await store.refreshHandoffs({ force: true });
  assert.equal(job.status, 'handed_off');
  assert.equal(job.workbuddyReadState, 'unavailable');
  assert.match(job.result, /user\.localassistant\.readable/);
});
