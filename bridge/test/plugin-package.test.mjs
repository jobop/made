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
  const config = {
    configDirectory: directory,
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

test('保存配置写回插件目录，并拒绝规范之外的值', async (t) => {
  const { config, pkg } = fixture(t);
  config.pluginsRuntime = await loadConfiguredPlugins(config);
  const saved = await savePluginSettings(config, 'alarm-clock', { demoSeconds: '', label: '起床' });
  assert.equal(saved.plugins[0].settings.label, '起床');
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(pkg, 'settings.json'), 'utf8')), { demoSeconds: '', label: '起床' });
  assert.equal(config.pluginSettings['alarm-clock'].label, '起床');
  await assert.rejects(savePluginSettings(config, 'alarm-clock', { demoSeconds: '08:00', label: '起床' }), /插件配置无效/);
  await assert.rejects(savePluginSettings(config, 'alarm-clock', { demoSeconds: '', label: '起床', extra: '1' }), /插件配置无效/);
});
