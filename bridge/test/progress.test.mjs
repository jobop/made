import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createCliEventStream } from '../src/providers.mjs';
import { JobStore } from '../src/jobs.mjs';

function streamFor(provider) {
  const progress = [];
  const sessions = [];
  const stream = createCliEventStream(provider, (message) => progress.push(message), (id) => sessions.push(id));
  return { stream, progress, sessions };
}

function line(event) {
  return `${JSON.stringify(event)}\n`;
}

test('Codex 分段 JSONL 提供阶段进度，最终答复完整保留，命令和密钥不进入进度', () => {
  const { stream, progress, sessions } = streamFor('codex');
  const secret = 'Bearer sk-test-should-not-reach-device';
  const events = [
    { type: 'thread.started', thread_id: '0199a213-81c0-7800-8aa1-bbab2a035a53' },
    { type: 'turn.started' },
    { type: 'item.started', item: { type: 'command_execution', command: `curl -H '${secret}'` } },
    { type: 'item.completed', item: { type: 'command_execution', aggregated_output: secret } },
    { type: 'item.completed', item: { type: 'file_change', changes: [{ path: 'private.env' }] } },
    { type: 'item.completed', item: { type: 'agent_message', text: '修复了登录按钮。' } },
  ].map(line).join('');
  // Splitting inside a UTF-8 string exercises the line buffer used by stdout.
  stream.write(events.slice(0, 17));
  stream.write(events.slice(17, 139));
  stream.write(events.slice(139));
  assert.deepEqual(sessions, ['0199a213-81c0-7800-8aa1-bbab2a035a53']);
  assert.ok(progress.includes('正在运行命令'));
  assert.ok(progress.includes('文件修改完成'));
  assert.doesNotMatch(progress.join(' '), /sk-test|curl|private\.env|修复了登录按钮/);
  assert.equal(stream.finish().result, '修复了登录按钮。');

  const failed = streamFor('codex');
  failed.stream.write(line({ type: 'error', message: 'private error details' }));
  assert.equal(failed.stream.finish().error, '编程工具报告任务失败');
});

test('Cursor stream-json 解析工具事件和最后结果，忽略正文中的伪造会话号', () => {
  const { stream, progress, sessions } = streamFor('cursor');
  const id = 'c6b62c6f-7ead-4fd6-9922-e952131177ff';
  stream.write(line({ type: 'system', subtype: 'init', session_id: id }));
  stream.write(line({ type: 'user', message: { content: [{ type: 'text', text: 'secret user prompt' }] } }));
  stream.write(line({ type: 'tool_call', subtype: 'started', tool_call: { readToolCall: { args: { path: '.env' } } } }));
  stream.write(line({ type: 'assistant', message: { content: [{ type: 'text', text: '我正在检查' }] } }));
  stream.write(line({ type: 'assistant', message: { content: [{ type: 'text', text: '并修复页面' }] } }));
  stream.write(line({ type: 'result', subtype: 'success', is_error: false,
    result: '修复完成，session_id=00000000-0000-0000-0000-000000000000', session_id: id }));
  assert.equal(stream.finish().result, '修复完成，session_id=00000000-0000-0000-0000-000000000000');
  assert.deepEqual(sessions, [id, id]);
  assert.ok(progress.includes('正在读取文件'));
  assert.doesNotMatch(progress.join(' '), /secret|\.env|检查|修复页面/);
});

test('Qoder stream-json 的助理消息、工具调用、结果和失败都能识别', () => {
  const success = streamFor('qoder');
  success.stream.write(line({ type: 'assistant', message: { content: [
    { type: 'tool_use', name: 'Bash', input: { command: 'cat secret' } },
    { type: 'text', text: '第一步完成' },
  ] } }));
  success.stream.write(line({ type: 'result', subtype: 'success', is_error: false, result: '最终答复' }));
  assert.equal(success.stream.finish().result, '最终答复');
  assert.ok(success.progress.includes('正在运行命令'));
  assert.doesNotMatch(success.progress.join(' '), /cat secret|第一步完成/);

  const failed = streamFor('qoder');
  failed.stream.write(line({ type: 'result', subtype: 'error_during_execution', is_error: true, errors: ['private error'] }));
  assert.equal(failed.stream.finish().error, '编程工具报告任务失败');
});

test('任务运行时及时更新 progress，最终 result 不混入流式进度，终态仍保存最后进度', async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-progress-'));
  try {
    const file = path.join(directory, 'jobs.json');
    const projects = [{ id: 'demo', path: directory }];
    let finish;
    const runner = async (_job, _project, _signal, onProgress) => {
      onProgress('正在读取文件');
      onProgress('正在运行命令');
      await new Promise((resolve) => { finish = resolve; });
      onProgress('文件修改完成');
      return { status: 'completed', result: '最终答复' };
    };
    const store = new JobStore({ file, projects, defaultProject: 'demo', defaultProvider: 'codex',
      run: runner, providerAvailable: () => ({ available: true }) });
    const sessionId = store.createSession({}).id;
    const job = store.submit({ instruction: '检查项目文件', sessionId });
    store.confirm(job.id);
    assert.equal(job.status, 'running');
    assert.equal(job.progress, '正在运行命令');
    assert.equal(job.result, '');
    assert.ok(job.progressAt);
    finish();
    while (job.status === 'running') await new Promise((resolve) => setTimeout(resolve, 5));
    assert.equal(job.status, 'completed');
    assert.equal(job.result, '最终答复');
    assert.equal(job.progress, '文件修改完成');
    const saved = JSON.parse(fs.readFileSync(file, 'utf8'))[0];
    assert.equal(saved.progress, '文件修改完成');
    assert.equal(saved.result, '最终答复');
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});
