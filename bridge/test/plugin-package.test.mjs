import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { loadConfiguredPlugins } from '../src/plugins/registry.mjs';
import { listInstalled, savePluginSettings } from '../src/installed.mjs';

function fixture(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-plugin-package-'));
  const pkg = path.join(directory, 'plugins', 'alarm-clock');
  fs.mkdirSync(pkg, { recursive: true });
  fs.writeFileSync(path.join(pkg, 'plugin.mjs'), `export default {
    apiVersion: 1, kind: 'board-plugin', id: 'alarm-clock', label: '闹钟',
    events: ['boot.double'], commands: ['caption.show'],
    probe: () => ({ available: true }), onEvent: () => ({ commands: [] })
  };`);
  fs.writeFileSync(path.join(pkg, 'plugin.json'), JSON.stringify({
    id: 'alarm-clock',
    settings: [
      { key: 'demoSeconds', label: '演示延迟（秒）', type: 'text', help: '留空则改用每天时间。', pattern: '^(?:|[1-9]\\d{0,3})$' },
      { key: 'label', label: '响铃文字', type: 'text', default: '闹钟' },
    ],
  }));
  fs.writeFileSync(path.join(pkg, 'settings.json'), JSON.stringify({ demoSeconds: '45' }));
  // 内置编程助手已插件化：默认助手 codex 由用户目录里的插件包提供。
  fs.writeFileSync(path.join(directory, 'plugins', 'codex.mjs'), `export default {
    apiVersion: 1, kind: 'coding-agent', id: 'codex', label: 'Codex',
    capabilities: { session: 'native', model: true, cancel: true, progress: true },
    probe: () => ({ available: true }), run: async () => ({ status: 'completed', result: '完成' })
  };`);
  const config = {
    configDirectory: directory,
    // 用户包目录固定为 ~/made，测试里指向临时目录。
    madeHome: directory,
    configFile: path.join(directory, 'config.local.json'),
    plugins: [],
    disabledPlugins: [],
    pluginSettings: { 'alarm-clock': { demoSeconds: '9', label: '旧配置' } },
    defaultProvider: 'codex',
  };
  fs.writeFileSync(config.configFile, JSON.stringify({ plugins: [], disabledPlugins: [] }));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return { config, pkg };
}

test('插件目录的配置规范覆盖桥接器里的同名 pluginSettings', async (t) => {
  const { config } = fixture(t);
  await loadConfiguredPlugins(config);
  assert.deepEqual(config.pluginSettings['alarm-clock'], { demoSeconds: '45', label: '闹钟' });
  const listed = await listInstalled(config);
  assert.equal(listed.plugins[0].file, 'alarm-clock');
  assert.deepEqual(listed.plugins[0].settingsSpec.map((field) => field.key), ['demoSeconds', 'label']);
  assert.equal(listed.plugins[0].settings.label, '闹钟');
});

test('保存配置写回插件目录，并保留开发者另写的自定义项', async (t) => {
  const { config, pkg } = fixture(t);
  config.pluginsRuntime = await loadConfiguredPlugins(config);
  const saved = await savePluginSettings(config, 'alarm-clock', { demoSeconds: '', label: '起床', extra: '1' });
  assert.equal(saved.plugins[0].settings.label, '起床');
  assert.equal(saved.plugins[0].settings.extra, '1');
  assert.equal(saved.plugins[0].settingsError, '');
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(pkg, 'settings.json'), 'utf8')), { demoSeconds: '', label: '起床', extra: '1' });
  assert.equal(config.pluginSettings['alarm-clock'].label, '起床');
  const again = await savePluginSettings(config, 'alarm-clock', { demoSeconds: '', label: '再改' });
  assert.equal(again.plugins[0].settings.extra, '1');
  assert.equal(JSON.parse(fs.readFileSync(path.join(pkg, 'settings.json'), 'utf8')).extra, '1');
  await assert.rejects(savePluginSettings(config, 'alarm-clock', { demoSeconds: '08:00', label: '起床' }), /插件配置无效/);
});

test('settings.json 里规范外的自定义项不让整张配置表失效', async (t) => {
  const { config } = fixture(t);
  fs.writeFileSync(path.join(config.madeHome, 'plugins', 'alarm-clock', 'settings.json'), JSON.stringify({ demoSeconds: '45', imageApiKeySiliconflow: 'sk-test' }));
  await loadConfiguredPlugins(config);
  const plugin = (await listInstalled(config)).plugins.find((item) => item.id === 'alarm-clock');
  assert.equal(plugin.settingsError, '');
  assert.deepEqual(plugin.settingsSpec.map((field) => field.key), ['demoSeconds', 'label']);
  assert.equal(plugin.settings.imageApiKeySiliconflow, 'sk-test');
  assert.equal(plugin.settings.label, '闹钟');
});

// ---- 云端资源环境变量占位符契约（${MADE_LLM_*}） ----

test('resolveSettings：default 里的 ${MADE_LLM_*} 占位符按白名单解析；非白名单变量保持原样', async () => {
  const { resolveSettings } = await import('../src/plugin-package.mjs');
  const fields = [
    { key: 'endpoint', label: '端点', type: 'text', default: '${MADE_LLM_BASE_URL}' },
    { key: 'apiKey', label: '密钥', type: 'text', default: '${MADE_LLM_TOKEN}' },
    { key: 'model', label: '模型', type: 'text', default: '${MADE_LLM_MODEL}' },
    { key: 'secret', label: '路径', type: 'text', default: '${HOME}/x' },
  ];
  const env = { MADE_LLM_BASE_URL: 'https://cloud.test/llm', MADE_LLM_TOKEN: 'tok-1', MADE_LLM_MODEL: 'm-1' };
  const values = resolveSettings(fields, {}, env);
  assert.deepEqual(
    { endpoint: values.endpoint, apiKey: values.apiKey, model: values.model },
    { endpoint: 'https://cloud.test/llm', apiKey: 'tok-1', model: 'm-1' },
  );
  // 白名单之外不解析（防止把沙箱环境变量泄漏进用户可见配置）
  assert.equal(values.secret, '${HOME}/x');
  // 云端环境缺失（本地跑）时保留占位符原样
  const local = resolveSettings(fields, {}, {});
  assert.equal(local.endpoint, '${MADE_LLM_BASE_URL}');
});

test('resolveSettings：用户显式值优先；显式空串回落 default；占位符解析后才校验 pattern', async () => {
  const { resolveSettings } = await import('../src/plugin-package.mjs');
  const fields = [
    { key: 'endpoint', label: '端点', type: 'text', default: '${MADE_LLM_BASE_URL}', pattern: '^https://' },
    { key: 'apiKey', label: '密钥', type: 'text', default: '${MADE_LLM_TOKEN}' },
  ];
  const env = { MADE_LLM_BASE_URL: 'https://cloud.test/llm', MADE_LLM_TOKEN: 'tok-1' };
  // 用户填自己的 → 原样
  const own = resolveSettings(fields, { endpoint: 'https://api.other.cn/v1', apiKey: 'sk-me' }, env);
  assert.equal(own.endpoint, 'https://api.other.cn/v1');
  // 显式空串=未填 → 回落 default 再解析
  const blank = resolveSettings(fields, { endpoint: '', apiKey: '' }, env);
  assert.equal(blank.endpoint, 'https://cloud.test/llm');
  assert.equal(blank.apiKey, 'tok-1');
  // 占位符解析后的值能过 URL pattern；解析不了的（环境缺失）回落原样也不会误伤合法 default
  assert.doesNotThrow(() => resolveSettings(fields, {}, env));
});

test('readPackageSettings 返回 saved（用户原始保存值）；writePackageSettings 落盘提交值、返回解析视图', async (t) => {
  const { readPackageSettings, writePackageSettings } = await import('../src/plugin-package.mjs');
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-cloudenv-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const pkg = path.join(directory, 'mimi');
  fs.mkdirSync(pkg);
  fs.writeFileSync(path.join(pkg, 'plugin.json'), JSON.stringify({
    id: 'mimi',
    settings: [
      { key: 'endpoint', label: '端点', type: 'text', default: '${MADE_LLM_BASE_URL}' },
      { key: 'apiKey', label: '密钥', type: 'text', default: '${MADE_LLM_TOKEN}' },
    ],
  }));
  const env = { MADE_LLM_BASE_URL: 'https://cloud.test/llm', MADE_LLM_TOKEN: 'tok-1' };
  // 无 settings.json：settings 全部来自 default 解析，saved 为空
  const fresh = readPackageSettings(pkg, env);
  assert.deepEqual(fresh.values, { endpoint: 'https://cloud.test/llm', apiKey: 'tok-1' });
  assert.deepEqual(fresh.saved, {});
  // 用户保存自己的值：values 用用户的，saved 留痕
  fs.writeFileSync(path.join(pkg, 'settings.json'), JSON.stringify({ endpoint: 'https://api.other.cn/v1' }));
  const savedView = readPackageSettings(pkg, env);
  assert.equal(savedView.values.endpoint, 'https://api.other.cn/v1');
  assert.equal(savedView.values.apiKey, 'tok-1');
  assert.deepEqual(savedView.saved, { endpoint: 'https://api.other.cn/v1' });
  // 保存提交（含空串=未填）：落盘提交值原样（不固化解析值），返回运行时视图
  const resolved = writePackageSettings(pkg, { endpoint: '', apiKey: '' }, env);
  assert.deepEqual(resolved, { endpoint: 'https://cloud.test/llm', apiKey: 'tok-1' });
  const onDisk = JSON.parse(fs.readFileSync(path.join(pkg, 'settings.json'), 'utf8'));
  assert.deepEqual(onDisk, { endpoint: '', apiKey: '' }, '空串保留空串，不把云端值写死进配置');
  // 重读仍能得到云端解析值
  assert.deepEqual(readPackageSettings(pkg, env).values, { endpoint: 'https://cloud.test/llm', apiKey: 'tok-1' });
});

// ---- 云端通道令牌轮换：不许把旧令牌固化在 settings.json ----
// 真实故障：工作台把解析后的令牌回显并提交保存 → settings.json 存下 JWT →
// 云端重启/沙箱重建换令牌后，助手仍拿旧令牌请求网关，模型侧 401。
const oldJwt = 'eyJhbGciOiJIUzI1NiJ9.eyJzY29wZSI6InNhbmRib3gifQ.old';
const newJwt = 'eyJhbGciOiJIUzI1NiJ9.eyJzY29wZSI6InNhbmRib3gifQ.new';
const cloudFields = [
  { key: 'endpoint', label: '端点', type: 'text', default: '${MADE_LLM_BASE_URL}' },
  { key: 'apiKey', label: '密钥', type: 'text', default: '${MADE_LLM_TOKEN}' },
  { key: 'model', label: '模型', type: 'text', default: '${MADE_LLM_MODEL}' },
];

test('resolveSettings：settings.json 里固化的旧沙箱令牌自动回落当前云端值；用户自定义密钥不受影响', async () => {
  const { resolveSettings, isSandboxJwt } = await import('../src/plugin-package.mjs');
  const env = { MADE_LLM_BASE_URL: 'https://cloud.test/llm', MADE_LLM_TOKEN: newJwt, MADE_LLM_MODEL: 'm-2' };
  assert.equal(isSandboxJwt(oldJwt), true);
  const healed = resolveSettings(cloudFields, { apiKey: oldJwt }, env);
  assert.equal(healed.apiKey, newJwt);
  assert.equal(healed.model, 'm-2');
  const own = resolveSettings(cloudFields, { apiKey: 'sk-my-own' }, env);
  assert.equal(own.apiKey, 'sk-my-own');
  // 云端环境缺失（本地跑）不能把用户值吞掉
  assert.equal(resolveSettings(cloudFields, { apiKey: oldJwt }, {}).apiKey, oldJwt);
});

test('writePackageSettings：提交解析值或令牌形状一律归一为「未填」，不固化凭证', async (t) => {
  const { writePackageSettings, readPackageSettings } = await import('../src/plugin-package.mjs');
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-token-heal-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const pkg = path.join(directory, 'mimi');
  fs.mkdirSync(pkg);
  fs.writeFileSync(path.join(pkg, 'plugin.json'), JSON.stringify({ id: 'mimi', settings: cloudFields }));
  const env = { MADE_LLM_BASE_URL: 'https://cloud.test/llm', MADE_LLM_TOKEN: newJwt, MADE_LLM_MODEL: 'm-2' };
  // 旧版 UI 把整张解析视图回显提交
  const resolved = writePackageSettings(pkg, { endpoint: 'https://cloud.test/llm', apiKey: newJwt, model: 'm-2' }, env);
  assert.equal(resolved.apiKey, newJwt);
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(pkg, 'settings.json'), 'utf8')), { endpoint: '', apiKey: '', model: '' });
  // 甚至直接提交一个陈旧令牌也不会落盘
  writePackageSettings(pkg, { endpoint: '', apiKey: oldJwt, model: '' }, env);
  assert.equal(JSON.parse(fs.readFileSync(path.join(pkg, 'settings.json'), 'utf8')).apiKey, '');
  // 存量固化文件重读时自动用上新令牌
  fs.writeFileSync(path.join(pkg, 'settings.json'), JSON.stringify({ apiKey: oldJwt }));
  assert.equal(readPackageSettings(pkg, env).values.apiKey, newJwt);
});

test('工作台视图不回显云端令牌，标为「云端提供」', async (t) => {
  const { config, pkg } = fixture(t);
  const previous = { ...process.env };
  process.env.MADE_LLM_BASE_URL = 'https://cloud.test/llm';
  process.env.MADE_LLM_TOKEN = newJwt;
  process.env.MADE_LLM_MODEL = 'm-2';
  t.after(() => {
    for (const key of ['MADE_LLM_BASE_URL', 'MADE_LLM_TOKEN', 'MADE_LLM_MODEL']) {
      if (previous[key] === undefined) delete process.env[key];
      else process.env[key] = previous[key];
    }
  });
  fs.writeFileSync(path.join(pkg, 'plugin.json'), JSON.stringify({ id: 'alarm-clock', settings: cloudFields }));
  fs.writeFileSync(path.join(pkg, 'settings.json'), JSON.stringify({ endpoint: '', apiKey: newJwt, model: '' }));
  const listed = await listInstalled(config);
  const plugin = listed.plugins.find((item) => item.file === 'alarm-clock');
  assert.equal(plugin.settingsError, '');
  assert.equal(plugin.settings.apiKey, '', '绝不把真实令牌发给工作台');
  assert.equal(plugin.settings.endpoint, '');
  assert.equal(plugin.settingsSources.apiKey, 'cloud');
});
