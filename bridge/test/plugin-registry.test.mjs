import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import dgram from 'node:dgram';
import { createPluginRegistry, loadConfiguredPlugins, validatePlugin } from '../src/plugins/registry.mjs';
import { loadConfig } from '../src/config.mjs';
import { createApp } from '../src/server.mjs';
import { providerStates } from '../src/providers.mjs';
import { builtinAgentIcons } from '../src/agent-icons.mjs';

function agent(extra = {}) {
  return {
    apiVersion: 1, kind: 'coding-agent', id: 'fake_agent', label: '测试助手', icon: 'owl',
    capabilities: { session: 'native', model: true, cancel: true, progress: true },
    model: { default: 'fake-default', required: true },
    probe: () => ({ available: true, mode: 'fake', reason: '' }),
    run: async () => ({ status: 'completed', result: '完成' }),
    ...extra,
  };
}

function temporary(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-plugin-registry-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return directory;
}

test('插件注册验证版本、类型、能力、重名与圆屏助手数量上限', () => {
  const good = agent();
  assert.equal(validatePlugin(good), good);
  for (const plugin of [
    agent({ apiVersion: 2 }), agent({ kind: 'unknown' }), agent({ id: 'constructor' }),
    agent({ id: '../escaped' }), agent({ id: 'bad space' }), agent({ label: '\u0000name' }),
    agent({ run: null }), agent({ probe: null }), agent({ icon: 'raw-html' }),
    agent({ capabilities: { ...good.capabilities, session: 'invented' } }),
    agent({ capabilities: { ...good.capabilities, cancel: 'yes' } }),
    agent({ sessionIdPattern: /id/g }), agent({ model: { default: 1, required: true } }),
    agent({ readUpdates: true }),
  ]) assert.throws(() => validatePlugin(plugin));
  assert.throws(() => createPluginRegistry([good, agent()]), /重复/);
  assert.throws(() => createPluginRegistry(Array.from({ length: 13 }, (_, i) => agent({ id: `agent_${i}` }))), /12/);
  const speech = { apiVersion: 1, kind: 'speech-recognizer', id: good.id, label: '测试语音', probe: () => ({ available: true }), fields: [], transcribe: async () => '' };
  assert.throws(() => createPluginRegistry([good, speech]), /重复/);
  assert.throws(() => validatePlugin({ ...speech, fields: [{ key: 'key', label: '密钥', type: 'secret', default: 'do-not-save' }] }));
});

test('状态输出只公开白名单字段，同步 probe 失败或返回 Promise 不视为就绪', async () => {
  const good = agent({ probe: () => ({ available: true, reason: '已就绪', apiKey: 'never-expose', token: 'private-token' }) });
  const malformed = agent({ id: 'malformed', probe: () => ({ available: 'true' }) });
  const asyncProbe = agent({ id: 'async_probe', probe: async () => { throw new Error('private-key'); } });
  const broken = agent({ id: 'broken', probe: () => { throw new Error('private-key'); } });
  const states = providerStates({ pluginsRuntime: createPluginRegistry([good, malformed, asyncProbe, broken]) });
  assert.deepEqual(states.map(state => state.available), [true, false, false, false]);
  assert.doesNotMatch(JSON.stringify(states), /never-expose|private-token|private-key|apiKey/);
  await new Promise(resolve => setImmediate(resolve));
});

test('显式本地模块可加载助手插件，禁用和默认助手验证有效，远程模块拒绝', async (t) => {
  const directory = temporary(t);
  fs.writeFileSync(path.join(directory, 'extension.mjs'), `export default [
    { apiVersion: 1, kind: 'coding-agent', id: 'community_agent', label: '社区助手', icon: 'rabbit',
      capabilities: {session:'native', model:true, cancel:true, progress:true}, model:{default:'test',required:false},
      probe:()=>({available:true}), run:async()=>({status:'completed',result:'测试'}) }
  ];`);
  const config = { configDirectory: directory, plugins: ['./extension.mjs'], disabledPlugins: ['codex', 'cursor', 'qoder', 'workbuddy'], defaultProvider: 'community_agent' };
  const loaded = await loadConfiguredPlugins(config);
  assert.deepEqual(loaded.agents().map(p => p.id), ['community_agent']);
  assert.deepEqual(loaded.speechRecognizers().map(p => p.id), ['openai', 'whisper', 'off']);
  await assert.rejects(loadConfiguredPlugins({ ...config, disabledPlugins: [...config.disabledPlugins, 'community_agent'] }), /默认/);
  await assert.rejects(loadConfiguredPlugins({ ...config, plugins: ['https://example.invalid/plugin.mjs'] }), /本地模块/);
  await assert.rejects(loadConfiguredPlugins({ ...config, plugins: ['./extension.mjs', './extension.mjs'] }), /重复/);
  fs.writeFileSync(path.join(directory, 'bad-version.mjs'), `export default {apiVersion: 2, id:'future'};`);
  await assert.rejects(loadConfiguredPlugins({ ...config, plugins: ['./bad-version.mjs'] }), /apiVersion/);
});

test('外部语音插件拒绝加载，包括禁用条目，内置语音接口不能被插件配置禁用', async (t) => {
  const directory = temporary(t);
  fs.writeFileSync(path.join(directory, 'speech.mjs'), `export default {
    apiVersion:1, kind:'speech-recognizer', id:'community_speech', label:'社区语音', fields:[],
    probe:()=>({available:true}), transcribe:async()=> '转写'
  };`);
  const config = { configDirectory: directory, plugins: ['./speech.mjs'], defaultProvider: 'codex' };
  await assert.rejects(loadConfiguredPlugins(config), /外部插件仅支持 coding-agent.*URL.*API Key.*模型名/);
  await assert.rejects(loadConfiguredPlugins({ ...config, disabledPlugins: ['community_speech'] }), /外部插件仅支持 coding-agent/);
  for (const id of ['openai', 'whisper', 'off']) {
    await assert.rejects(loadConfiguredPlugins({ ...config, plugins: [], disabledPlugins: [id] }), /内置接口.*不能.*禁用/);
  }
  const loaded = await loadConfiguredPlugins({ ...config, plugins: [], disabledPlugins: ['cursor'] });
  assert.equal(loaded.hasAgent('cursor'), false);
  assert.equal(loaded.hasSpeechRecognizer('openai'), true);
});

async function freeTcpPort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
  const port = server.address().port;
  await new Promise(resolve => server.close(resolve));
  return port;
}
async function freeUdpPort() {
  const socket = dgram.createSocket('udp4');
  await new Promise((resolve, reject) => { socket.once('error', reject); socket.bind(0, '127.0.0.1', resolve); });
  const port = socket.address().port;
  socket.close();
  return port;
}
async function until(check) {
  const deadline = Date.now() + 2000;
  while (!check()) {
    if (Date.now() > deadline) throw new Error('测试助手未完成');
    await new Promise(resolve => setTimeout(resolve, 5));
  }
}

test('本地模块助手贯通圆屏配置、会话接口、模型保存和后续轮次，未调用真实 CLI', async (t) => {
  const directory = temporary(t);
  fs.writeFileSync(path.join(directory, 'community.mjs'), `export default {
    apiVersion:1, kind:'coding-agent', id:'9_community', label:'社区测试', icon:'rabbit',
    capabilities:{session:'native',model:true,cancel:true,progress:true}, model:{default:'fake-small',required:true},
    probe:()=>({available:true, mode:'fake', secret:'do-not-expose'}),
    async run({job,model,onSession,onProgress}) {
      const previous = job.sessionId || null;
      onSession('community:context'); onProgress('正在测试');
      return {status:'completed',result:JSON.stringify({model,previous})};
    }
  };`);
  const configPath = path.join(directory, 'config.json');
  fs.writeFileSync(configPath, JSON.stringify({ projects: [{ id: 'demo', label: '项目'.repeat(20000), path: directory }, { id: 'other', path: directory }], defaultProvider: '9_community', plugins: ['./community.mjs'], disabledPlugins: ['codex', 'cursor', 'qoder', 'workbuddy'] }));
  const env = { VIBE_CONFIG: configPath, VIBE_STATE_DIR: path.join(directory, 'state'), VIBE_ASR_MODE: 'off' };
  const config = loadConfig(env);
  config.pluginsRuntime = await loadConfiguredPlugins(config);
  config.port = await freeTcpPort();
  do { config.devicePort = await freeTcpPort(); } while (config.port === config.devicePort);
  config.discoveryPort = await freeUdpPort();
  const app = createApp(config);
  await app.listen();
  t.after(() => app.close());
  const desktop = `http://127.0.0.1:${config.port}`;
  const device = `http://127.0.0.1:${config.devicePort}`;
  const { token } = await (await fetch(`${desktop}/api/session`)).json();
  const post = (endpoint, payload, auth = token) => fetch(`${desktop}${endpoint}`, {
    method: 'POST', headers: { 'content-type': 'application/json', 'x-vibe-token': auth }, body: JSON.stringify(payload),
  });
  assert.equal((await post('/api/settings/models', { '9_community': 'fake-large' }, '')).status, 403);
  assert.equal((await post('/api/settings/models', { '9_community': '' })).status, 400);
  const saved = await post('/api/settings/models', { '9_community': 'fake-large' });
  assert.equal(saved.status, 200);
  assert.deepEqual((await saved.json()).models, { '9_community': 'fake-large' });
  assert.equal(loadConfig(env).models['9_community'], 'fake-large');
  assert.equal((await post('/api/settings/models', { '9_community': 'fake-large', codex: 'should-reject' })).status, 400);

  const deviceId = '010203040506';
  const nonce = 'a'.repeat(32);
  app.pairing.open();
  app.pairing.request({ deviceId, deviceName: '测试圆屏', code: '123456', nonce }, '127.0.0.1', `127.0.0.1:${config.devicePort}`);
  app.pairing.decide(deviceId, nonce, 'confirm');
  const deviceToken = app.pairing.paired.get(deviceId).token;
  const deviceHeaders = { Authorization: `Bearer ${deviceToken}`, 'content-type': 'application/json' };
  assert.equal((await fetch(`${device}/device/config`)).status, 401);
  const deviceConfig = await (await fetch(`${device}/device/config`, { headers: deviceHeaders })).json();
  assert.equal(deviceConfig.providers.length, 1);
  assert.equal(deviceConfig.providers[0].id, '9_community');
  assert.equal(deviceConfig.providers[0].label, '社区测试');
  assert.deepEqual(deviceConfig.projects.map(project => project.id), ['demo']);
  assert.ok(Buffer.byteLength(deviceConfig.projects[0].label) <= 64);
  assert.ok(Buffer.byteLength(JSON.stringify(deviceConfig)) <= 32 * 1024);
  assert.deepEqual(deviceConfig.providers[0].icon, builtinAgentIcons.rabbit);
  assert.equal(deviceConfig.providers[0].capabilities.model, true);
  assert.doesNotMatch(JSON.stringify(deviceConfig), /do-not-expose|secret/);
  const created = await fetch(`${device}/device/sessions?provider=9_community`, { method: 'POST', headers: deviceHeaders, body: '{}' });
  assert.equal(created.status, 201);
  const session = await created.json();
  for (let i = 0; i < 2; i++) {
    const submitted = await post('/api/jobs', { sessionId: session.id, instruction: '测试同一任务持续对话' });
    assert.equal(submitted.status, 201);
    const job = await submitted.json();
    assert.equal((await post(`/api/jobs/${job.id}/confirm`, {})).status, 200);
    await until(() => app.store.get(job.id).status === 'completed');
    assert.deepEqual(JSON.parse(app.store.get(job.id).result), { model: 'fake-large', previous: i === 0 ? null : 'community:context' });
  }
  const sessions = await (await fetch(`${device}/device/sessions?provider=9_community`, { headers: deviceHeaders })).json();
  assert.equal(sessions.sessions.length, 1);
  assert.equal((await fetch(`${device}/device/tasks?provider=9_community`, { headers: deviceHeaders })).status, 200);
  const desktopState = await (await fetch(`${desktop}/api/state`)).json();
  assert.equal(desktopState.projects.length, 2);
  assert.equal(desktopState.projects[0].label, '项目'.repeat(20000));
  assert.doesNotMatch(JSON.stringify(desktopState.providers), /do-not-expose|secret/);
});
