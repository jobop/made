// 工具调用轨迹：只进桥接器任务记录（网页端折叠展示），参数绝不进入设备进度文本。
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { jsonLineEvents } from '../src/agents/events.mjs';
import { JobStore } from '../src/jobs.mjs';
import { testRegistry } from './helpers/fixture-agents.mjs';

const line = (event) => `${JSON.stringify(event)}\n`;

test('jsonLineEvents 捕获工具调用与参数：started/completed 合并，密钥不进进度', () => {
  const progress = [];
  const stream = jsonLineEvents((event, state) => {
    if (event.item?.type === 'agent_message' && event.type === 'item.completed') state.lastMessage = event.item.text;
    return { activity: event.type === 'item.started' ? '正在运行命令' : '' };
  }, (message) => progress.push(message));
  stream.write([
    line({ type: 'item.started', item: { id: 'c1', type: 'command_execution', command: 'curl -H "Authorization: sk-secret-args"' } }),
    line({ type: 'item.completed', item: { id: 'c1', type: 'command_execution' } }),
    line({ type: 'item.completed', item: { id: 'f1', type: 'file_change', changes: [{ path: 'src/app.js', kind: 'modified' }] } }),
    line({ type: 'item.completed', item: { type: 'agent_message', text: '完成。' } }),
  ].join(''));
  assert.doesNotMatch(progress.join(' '), /sk-secret-args|curl|app\.js/);

  const calls = stream.takeToolCalls();
  assert.equal(calls.length, 2);
  assert.equal(calls[0].name, '命令执行');
  assert.match(calls[0].detail, /curl/);
  assert.equal(calls[0].state, 'completed');
  assert.equal(calls[1].name, '文件修改');
  assert.match(calls[1].detail, /src\/app\.js/);
  // takeToolCalls 清空缓冲，重复抽取不再返回。
  assert.deepEqual(stream.takeToolCalls(), []);
});

test('流末尾未完成的工具调用在 finish 时收口为完成', () => {
  const stream = jsonLineEvents(() => ({}), () => {});
  stream.write(line({ type: 'item.started', item: { id: 'w1', type: 'web_search', query: 'esp-idf api' } }));
  const calls = stream.takeToolCalls();
  assert.equal(calls.length, 1);
  stream.finish();
  assert.equal(calls[0].state, 'completed');
});

test('JobStore 把 onTool 调用记入任务并持久化，completed 合并 started', async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-tool-trace-'));
  try {
    const file = path.join(directory, 'jobs.json');
    const projects = [{ id: 'demo', path: directory }];
    const runner = async (_job, _project, _signal, _onProgress, _onSession, onTool) => {
      onTool({ name: '命令执行', detail: 'git status --porcelain', state: 'started' });
      onTool({ name: '命令执行', detail: 'git status --porcelain', state: 'completed' });
      onTool({ name: '文件修改', detail: 'src/main.js', state: 'completed' });
      return { status: 'completed', result: '最终答复' };
    };
    const store = new JobStore({ file, projects, defaultProject: 'demo', defaultProvider: 'codex',
      run: runner, pluginsRuntime: testRegistry(), providerAvailable: () => ({ available: true }) });
    const sessionId = store.createSession({}).id;
    const job = store.submit({ instruction: '检查项目文件', sessionId });
    store.confirm(job.id);
    while (job.status === 'running') await new Promise((resolve) => setTimeout(resolve, 5));
    assert.equal(job.status, 'completed');
    assert.equal(job.toolCalls.length, 2);
    assert.equal(job.toolCalls[0].name, '命令执行');
    assert.equal(job.toolCalls[0].state, 'completed');
    assert.equal(job.toolCalls[1].name, '文件修改');
    const saved = JSON.parse(fs.readFileSync(file, 'utf8'))[0];
    assert.equal(saved.toolCalls.length, 2);
    assert.equal(saved.toolCalls[0].detail, 'git status --porcelain');
  } finally {
    fs.rmSync(directory, { recursive: true, force: true });
  }
});
