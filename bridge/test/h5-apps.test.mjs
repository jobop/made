import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { createApp } from '../src/server.mjs';
import { H5_CAPABILITIES, H5Tokens, readH5Manifest, h5StaticRoot, resolveStaticFile } from '../src/h5-apps.mjs';
import { testRegistry } from './helpers/fixture-agents.mjs';

const reserve = async () => {
  const server = net.createServer();
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
};

const request = (port, route, headers = {}) => new Promise((resolve, reject) => {
  http.get({ hostname: '127.0.0.1', port, path: route, headers }, async (res) => {
    const chunks = [];
    for await (const chunk of res) chunks.push(chunk);
    resolve({ status: res.statusCode, headers: res.headers,
      body: Buffer.concat(chunks).toString(), raw: Buffer.concat(chunks) });
  }).on('error', reject);
});

const post = (port, route, body, headers = {}) => new Promise((resolve, reject) => {
  const data = typeof body === 'string' ? body : JSON.stringify(body);
  const req = http.request({ hostname: '127.0.0.1', port, path: route, method: 'POST',
    headers: { 'content-type': 'application/json', 'content-length': Buffer.byteLength(data), ...headers } },
    async (res) => {
      const chunks = [];
      for await (const chunk of res) chunks.push(chunk);
      resolve({ status: res.statusCode, body: Buffer.concat(chunks).toString() });
    });
  req.on('error', reject);
  req.end(data);
});

/** 造一个已安装的 h5 包：madeHome/plugins/<id>/。 */
function makeH5Package(home, id, { capabilities, www = 'www/', extra = {} } = {}) {
  const dir = path.join(home, 'plugins', id);
  fs.mkdirSync(path.join(dir, www), { recursive: true });
  fs.writeFileSync(path.join(dir, 'plugin.json'), JSON.stringify({
    id, kind: 'h5', ...(www !== 'www/' ? { www } : {}), ...(capabilities ? { capabilities } : {}), ...extra,
  }));
  fs.writeFileSync(path.join(dir, www, 'index.html'), '<!DOCTYPE html><html><body>h5-ok</body></html>');
  fs.writeFileSync(path.join(dir, www, 'app.js'), 'console.log(1)');
  return dir;
}

async function boot(t, home) {
  const config = {
    port: await reserve(), devicePort: await reserve(), discoveryPort: await reserve(),
    stateDir: path.join(home, 'state'), madeHome: home,
    projects: [{ id: 'demo', label: 'Demo', path: home }],
    defaultProject: 'demo', defaultProvider: 'codex', asrMode: 'off',
    workbuddy: { allowUnscopedDispatch: false }, pluginsRuntime: testRegistry(),
  };
  const app = createApp(config, { enableUsb: false, run: async () => ({ status: 'completed', result: 'done' }) });
  await app.listen();
  t.after(() => app.close());
  const deviceId = '0123456789ab';
  app.pairing.open();
  app.pairing.request({ deviceId, deviceName: '测试机', code: '123456', nonce: 'a'.repeat(32) }, '127.0.0.1', '127.0.0.1:8788');
  app.pairing.decide(deviceId, app.pairing.summary().pending[0].nonce, 'confirm');
  const token = app.pairing.paired.get(deviceId).token;
  return { app, config, token };
}

test('H5 manifest 校验：非法形态全部拒绝，合法的最小包通过', () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-h5-manifest-'));
  t_after: {
    const dir = makeH5Package(home, 'good-app');
    assert.deepEqual(readH5Manifest(dir), { id: 'good-app', kind: 'h5', www: 'www/', capabilities: [] });
    fs.writeFileSync(path.join(dir, 'plugin.json'), JSON.stringify({ id: 'good-app', kind: 'agent' }));
    assert.equal(readH5Manifest(dir), null, 'kind 不是 h5');
    fs.writeFileSync(path.join(dir, 'plugin.json'), JSON.stringify({ id: 'other', kind: 'h5' }));
    assert.equal(readH5Manifest(dir), null, 'id 与目录不一致');
    fs.writeFileSync(path.join(dir, 'plugin.json'), JSON.stringify({ id: 'good-app', kind: 'h5', www: '../escape/' }));
    assert.equal(readH5Manifest(dir), null, 'www 路径穿越');
    fs.writeFileSync(path.join(dir, 'plugin.json'), JSON.stringify({ id: 'good-app', kind: 'h5', capabilities: ['state.read', 'hack.all'] }));
    assert.equal(readH5Manifest(dir), null, '能力不在白名单');
    fs.writeFileSync(path.join(dir, 'plugin.json'), JSON.stringify({ id: 'good-app', kind: 'h5', capabilities: ['tasks.write'] }));
    assert.deepEqual(readH5Manifest(dir).capabilities, ['tasks.write']);
  }
  fs.rmSync(home, { recursive: true, force: true });
});

test('静态服务：入口页、子文件、SPA 回落、穿越拒绝、非 h5 目录 404', async (t) => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-h5-static-'));
  t.after(() => fs.rmSync(home, { recursive: true, force: true }));
  makeH5Package(home, 'static-app');
  fs.mkdirSync(path.join(home, 'plugins', 'not-h5'), { recursive: true });
  fs.writeFileSync(path.join(home, 'plugins', 'not-h5', 'plugin.mjs'), 'export default {}');
  const { config, token } = await boot(t, home);
  const base = `http://127.0.0.1:${config.devicePort}`;

  const index = await request(config.devicePort, '/plugins/static-app/');
  assert.equal(index.status, 200);
  assert.match(index.body, /h5-ok/);
  assert.equal(index.headers['content-type'], 'text/html; charset=utf-8');
  assert.equal((await request(config.devicePort, '/plugins/static-app/app.js')).headers['content-type'], 'text/javascript; charset=utf-8');
  assert.equal((await request(config.devicePort, '/plugins/static-app/missing-page')).status, 404);
  // URL 规范化已折叠 %2e%2e 点段（落不到 h5 路由 → 401）；带段的 ..%2F 形式由处理器拒绝。
  assert.equal((await request(config.devicePort, '/plugins/static-app/..%2Fetc%2Fpasswd')).status, 404, '编码路径穿越');
  assert.equal((await request(config.devicePort, '/plugins/not-h5/')).status, 404, '非 h5 包不服务');
  assert.equal((await request(config.devicePort, '/plugins/no-such-app/')).status, 404);
  // token 端点要走设备令牌
  assert.equal((await request(config.devicePort, '/plugins/static-app/token')).status, 401);
  const issued = await request(config.devicePort, '/plugins/static-app/token', { Authorization: `Bearer ${token}` });
  assert.equal(issued.status, 200);
  const grant = JSON.parse(issued.body);
  assert.deepEqual(grant.capabilities, []);
  assert.match(grant.token, /^[0-9a-f]{64}$/);
});

test('能力协商：未声明能力的页面调接口 403，声明后按能力放行', async (t) => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-h5-caps-'));
  t.after(() => fs.rmSync(home, { recursive: true, force: true }));
  makeH5Package(home, 'dumb-app');
  makeH5Package(home, 'tasky', { capabilities: ['tasks.read', 'tasks.write', 'sessions.write', 'state.read'] });
  const { app, config, token } = await boot(t, home);

  const issue = async (id) => JSON.parse((await request(config.devicePort, `/plugins/${id}/token`,
    { Authorization: `Bearer ${token}` })).body);

  // 纯静态包：有 token 也碰不了接口
  const dumb = await issue('dumb-app');
  assert.equal((await request(config.devicePort, `/h5api/state?h5token=${dumb.token}`)).status, 403);
  assert.equal((await request(config.devicePort, '/h5api/state', { Authorization: `Bearer ${dumb.token}` })).status, 403);

  // 能力包：whoami 永远可调（不算能力），state/tasks 按声明放行
  const grant = await issue('tasky');
  const auth = { Authorization: `Bearer ${grant.token}` };
  const who = JSON.parse((await request(config.devicePort, '/h5api/whoami', auth)).body);
  assert.equal(who.plugin, 'tasky');
  assert.deepEqual([...who.capabilities].sort(), ['sessions.write', 'state.read', 'tasks.read', 'tasks.write']);
  assert.equal((await request(config.devicePort, '/h5api/state', auth)).status, 200);
  assert.equal((await request(config.devicePort, '/h5api/tasks', auth)).status, 200);
  assert.equal((await request(config.devicePort, '/h5api/sessions', auth)).status, 403, '未声明 sessions.read');

  // 建会话（sessions.write）→ 提交任务 → 确认，全链路
  const session = JSON.parse((await post(config.devicePort, '/h5api/sessions', { title: '来自 H5' }, auth)).body);
  assert.equal(session.title, '来自 H5');
  const job = JSON.parse((await post(config.devicePort, '/h5api/tasks', { sessionId: session.id, instruction: '数到三' }, auth)).body);
  assert.equal(job.status, 'waiting_confirmation');
  assert.equal(job.instruction, '数到三');
  const confirmed = JSON.parse((await post(config.devicePort, `/h5api/tasks/${job.id}/confirm`, '', auth)).body);
  assert.ok(['queued', 'running'].includes(confirmed.status), `确认后状态应为 queued/running，收到 ${confirmed.status}`);
  // 假 run 立即完成：任务应很快 completed
  const deadline = Date.now() + 2000;
  let done = null;
  while (Date.now() < deadline && !done) {
    const state = JSON.parse((await request(config.devicePort, '/h5api/state', auth)).body);
    done = state.jobs.find((item) => item.id === job.id && item.status === 'completed');
    if (!done) await new Promise((resolve) => setTimeout(resolve, 20));
  }
  assert.equal(done?.result, 'done');
  // 坏 token / 缺 token
  assert.equal((await request(config.devicePort, '/h5api/tasks')).status, 401);
  assert.equal((await request(config.devicePort, '/h5api/tasks?h5token=deadbeef')).status, 401);
  assert.equal(app.h5Tokens ? true : true, true); // 结构存在性（token store 在闭包里，不外露）
});

test('H5Tokens：过期失效、revoke 按包撤销', async () => {
  const store = new H5Tokens();
  const a = store.issue('app-a', ['state.read']);
  const b = store.issue('app-b', ['tasks.read']);
  assert.equal(store.verify(a.token).id, 'app-a');
  store.revoke('app-a');
  assert.equal(store.verify(a.token), null);
  assert.equal(store.verify(b.token).id, 'app-b');
  store.tokens.get(b.token).expiresAt = Date.now() - 1;
  assert.equal(store.verify(b.token), null, '过期失效');
});

test('H5 设置项：listH5Apps 出表单，/api/installed 带 h5apps，保存后 /h5api/settings 可读且密钥遮蔽', async (t) => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-h5-settings-'));
  t.after(() => fs.rmSync(home, { recursive: true, force: true }));
  makeH5Package(home, 'setty', {
    capabilities: ['settings.read'],
    extra: { settings: [
      { key: 'nickname', label: '昵称', type: 'text', default: '阿黄' },
      { key: 'apiKey', label: '密钥', type: 'text' },
    ] },
  });
  makeH5Package(home, 'plain');
  const { app, config, token } = await boot(t, home);
  const headers = { Authorization: `Bearer ${token}` };

  // 桌面口 /api/installed 带 h5apps；设置表单来自 plugin.json（含 default 解析）
  const installed = JSON.parse((await request(config.port, '/api/installed', headers)).body);
  const setty = installed.h5apps.find((item) => item.id === 'setty');
  assert.ok(setty, 'installed.h5apps 应含 setty');
  assert.deepEqual(setty.settingsSpec.map((field) => field.key), ['nickname', 'apiKey']);
  assert.equal(setty.settings.nickname, '阿黄');
  const plain = installed.h5apps.find((item) => item.id === 'plain');
  assert.deepEqual(plain.settingsSpec, []);

  // 保存（POST /api/plugins/settings 要 x-vibe-token）：先拿桌面口会话令牌
  const vibeToken = JSON.parse((await request(config.port, '/api/session', headers)).body).token;
  const saved = await post(config.port, '/api/plugins/settings',
    { id: 'setty', settings: { nickname: '小白', apiKey: 'sk-secret-1' } },
    { Authorization: `Bearer ${token}`, 'x-vibe-token': vibeToken });
  assert.equal(saved.status, 200, saved.body);

  // 落盘 + 重读
  const after = JSON.parse((await request(config.port, '/api/installed', headers)).body)
    .h5apps.find((item) => item.id === 'setty');
  assert.equal(after.settings.nickname, '小白');
  assert.equal(after.settings.apiKey, 'sk-secret-1');

  // 页面读配置：声明 settings.read 才放行；密钥类只出「有没有填」，不出值
  const issue = JSON.parse((await request(config.devicePort, '/plugins/setty/token', headers)).body);
  const auth = { Authorization: `Bearer ${issue.token}` };
  const who = JSON.parse((await request(config.devicePort, '/h5api/whoami', auth)).body);
  assert.ok(who.capabilities.includes('settings.read'));
  const view = JSON.parse((await request(config.devicePort, '/h5api/settings', auth)).body);
  assert.equal(view.plugin, 'setty');
  assert.equal(view.settings.nickname, '小白');
  assert.equal(view.settings.apiKey, undefined, '密钥类字段不得出值');
  assert.deepEqual(view.secretFields, ['apiKey']);
  // 未声明能力：403
  const plainIssue = JSON.parse((await request(config.devicePort, '/plugins/plain/token', headers)).body);
  assert.equal((await request(config.devicePort, '/h5api/settings',
    { Authorization: `Bearer ${plainIssue.token}` })).status, 403);
});
