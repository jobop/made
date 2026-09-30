import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import dgram from 'node:dgram';
import { spawnSync } from 'node:child_process';
import { createApp } from '../src/server.mjs';
import { loadConfig } from '../src/config.mjs';
import { createRunner } from '../src/providers.mjs';
import { loadModelSettings, validateModelSettings } from '../src/model-settings.mjs';

async function availableTcpPort() {
  const server = net.createServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}

async function availableUdpPort() {
  const socket = dgram.createSocket('udp4');
  await new Promise((resolve) => socket.bind(0, '127.0.0.1', resolve));
  const port = socket.address().port;
  await new Promise((resolve) => socket.close(resolve));
  return port;
}

test('模型名称只接受 CLI 安全标识；Cursor/Qoder 可留空，Codex 不可留空', () => {
  assert.deepEqual(validateModelSettings({
    codex: 'gpt-6-astra', cursor: 'claude-opus-4-8[context=1m,effort=high]', qoder: '',
  }), { codex: 'gpt-6-astra', cursor: 'claude-opus-4-8[context=1m,effort=high]', qoder: '' });
  for (const value of [
    { codex: '', cursor: '', qoder: '' },
    { codex: 'gpt-6-astra', cursor: '--force', qoder: '' },
    { codex: 'gpt-6-astra', cursor: 'model; rm -rf /', qoder: '' },
    { codex: 'gpt-6-astra', cursor: '', qoder: 'model\n--yolo' },
    { codex: 'gpt-6-astra', cursor: '', qoder: '', workbuddy: 'secret' },
  ]) assert.throws(() => validateModelSettings(value));
});

test('模型设置接口需本机操作令牌、保存后可重启读取，WorkBuddy 固定由自身应用选择', async (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-model-settings-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const configPath = path.join(directory, 'config.json');
  fs.writeFileSync(configPath, JSON.stringify({
    defaultProject: 'demo', defaultProvider: 'codex',
    projects: [{ id: 'demo', path: directory }],
  }));
  const env = { VIBE_CONFIG: configPath, VIBE_STATE_DIR: path.join(directory, 'state'),
    VIBE_CODEX_MODEL: 'gpt-5.5' };
  const config = loadConfig(env);
  assert.deepEqual(config.models, { codex: 'gpt-5.5', cursor: '', qoder: '' });
  config.port = await availableTcpPort();
  config.devicePort = await availableTcpPort();
  config.discoveryPort = await availableUdpPort();
  const app = createApp(config, { run: async () => ({ status: 'completed', result: '完成' }) });
  await app.listen();
  t.after(async () => app.close());
  const base = `http://127.0.0.1:${config.port}`;
  const url = `${base}/api/settings/models`;
  const initial = await (await fetch(`${base}/api/state`)).json();
  assert.deepEqual(initial.models, { codex: 'gpt-5.5', cursor: '', qoder: '', workbuddy: null });
  const chosen = { codex: 'gpt-6-astra', cursor: 'claude-opus-4-8', qoder: 'auto' };
  const post = (payload, token) => fetch(url, {
    method: 'POST', headers: { 'Content-Type': 'application/json', 'x-vibe-token': token || '' },
    body: JSON.stringify(payload),
  });
  assert.equal((await post(chosen, '')).status, 403);
  const { token } = await (await fetch(`${base}/api/session`)).json();
  assert.equal((await post({ ...chosen, codex: '' }, token)).status, 400);
  assert.equal((await post({ ...chosen, workbuddy: 'auto' }, token)).status, 400);
  const saved = await post(chosen, token);
  assert.equal(saved.status, 200);
  assert.deepEqual((await saved.json()).models, { ...chosen, workbuddy: null });
  assert.deepEqual((await (await fetch(`${base}/api/state`)).json()).models,
    { ...chosen, workbuddy: null });
  assert.deepEqual(loadModelSettings(env.VIBE_STATE_DIR), chosen);
  assert.deepEqual(loadConfig(env).models, chosen);
  assert.equal(fs.statSync(path.join(env.VIBE_STATE_DIR, 'models.json')).mode & 0o777, 0o600);
  assert.equal((await post({ ...chosen, cursor: '', qoder: '' }, token)).status, 200);
  assert.equal(loadConfig(env).models.cursor, '');
});

test('每个 CLI 以独立参数接收模型；修改设置只影响之后启动的任务', async (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-model-cli-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const bin = path.join(directory, 'bin');
  const repoPath = path.join(directory, 'repo');
  fs.mkdirSync(bin);
  fs.mkdirSync(repoPath);
  const repo = fs.realpathSync(repoPath);
  assert.equal(spawnSync('git', ['init', '-q', repo]).status, 0);
  const marker = path.join(directory, 'args.jsonl');
  const script = `#!/usr/bin/env node
const fs = require('node:fs');
const path = require('node:path');
const provider = path.basename(process.argv[1]);
fs.appendFileSync(process.env.VIBE_TEST_MODEL_ARGS, JSON.stringify({ provider, args: process.argv.slice(2) }) + '\\n');
if (provider === 'codex') {
  process.stdout.write(JSON.stringify({ type: 'item.completed', item: { type: 'agent_message', text: 'Codex 完成' } }) + '\\n');
} else {
  setTimeout(() => process.stdout.write(JSON.stringify({ type: 'result', subtype: 'success', is_error: false, result: '完成' }) + '\\n'), 100);
}
`;
  for (const name of ['codex', 'cursor-agent', 'qoder']) {
    fs.writeFileSync(path.join(bin, name), script, { mode: 0o755 });
  }
  const priorPath = process.env.PATH;
  const priorMarker = process.env.VIBE_TEST_MODEL_ARGS;
  process.env.PATH = `${bin}${path.delimiter}${priorPath || ''}`;
  process.env.VIBE_TEST_MODEL_ARGS = marker;
  t.after(() => {
    process.env.PATH = priorPath;
    if (priorMarker === undefined) delete process.env.VIBE_TEST_MODEL_ARGS;
    else process.env.VIBE_TEST_MODEL_ARGS = priorMarker;
  });
  const config = { models: { codex: 'gpt-6-astra', cursor: 'cursor-old', qoder: 'qoder-old' } };
  const runner = createRunner(config);
  const run = (provider) => runner({ provider, instruction: '检查演示项目' }, { path: repo },
    new AbortController().signal, () => {}, () => {});
  await run('codex');
  await run('qoder');
  const cursorRunning = run('cursor');
  // Child arguments are captured at spawn. A later desktop edit changes only
  // tasks launched afterward.
  await new Promise((resolve) => setTimeout(resolve, 30));
  config.models.cursor = 'cursor-new';
  await cursorRunning;
  await run('cursor');
  config.models.cursor = '';
  config.models.qoder = '';
  await run('cursor');
  await run('qoder');
  const calls = fs.readFileSync(marker, 'utf8').trim().split('\n').map(JSON.parse);
  const option = (call, flag) => {
    const index = call.args.indexOf(flag);
    return index < 0 ? null : call.args[index + 1];
  };
  assert.equal(option(calls[0], '-m'), 'gpt-6-astra');
  assert.equal(option(calls[1], '--model'), 'qoder-old');
  assert.equal(option(calls[2], '--model'), 'cursor-old');
  assert.equal(option(calls[3], '--model'), 'cursor-new');
  assert.equal(option(calls[4], '--model'), null);
  assert.equal(option(calls[5], '--model'), null);
  assert.ok(calls.slice(1).every((call) => option(call, '--output-format') === 'stream-json'));
});
