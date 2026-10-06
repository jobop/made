import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { listInstalled, removePlugin, removeTheme, setPluginEnabled } from '../src/installed.mjs';
import { loadConfiguredPlugins } from '../src/plugins/registry.mjs';

function fixture(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-installed-'));
  const configFile = path.join(directory, 'config.local.json');
  fs.mkdirSync(path.join(directory, 'plugins'));
  fs.mkdirSync(path.join(directory, 'themes', 'bobo'), { recursive: true });
  fs.writeFileSync(path.join(directory, 'plugins', 'alarm.mjs'), `export default {
    apiVersion: 1, kind: 'board-plugin', id: 'alarm_clock', label: '闹钟',
    events: ['boot.double'], commands: ['caption.show'],
    probe: () => ({ available: true }), onEvent: () => ({ commands: [] })
  };`);
  fs.writeFileSync(path.join(directory, 'themes', 'bobo', 'theme.json'), JSON.stringify({ title: '波波' }));
  fs.writeFileSync(configFile, JSON.stringify({ plugins: [], disabledPlugins: [] }));
  const config = {
    configDirectory: directory,
    configFile,
    plugins: [],
    disabledPlugins: [],
    defaultProvider: 'codex',
  };
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return config;
}

test('插件页能看到目录里的插件和波波主题，停用会写入配置', async (t) => {
  const config = fixture(t);
  config.pluginsRuntime = await loadConfiguredPlugins(config);
  const listed = await listInstalled(config);
  assert.equal(listed.plugins[0].label, '闹钟');
  assert.equal(listed.plugins[0].enabled, true);
  assert.deepEqual(listed.themes, [{ name: 'bobo', title: '波波', sound: false, icon: false, background: false }]);
  const disabled = await setPluginEnabled(config, 'alarm_clock', false);
  assert.equal(disabled.plugins[0].enabled, false);
  assert.equal(config.pluginsRuntime.boardPlugin('alarm_clock'), undefined);
  assert.deepEqual(JSON.parse(fs.readFileSync(config.configFile, 'utf8')).disabledPlugins, ['alarm_clock']);
  await assert.rejects(setPluginEnabled(config, 'missing', true), /插件不存在/);
  assert.throws(() => removeTheme(config, '../secrets'), /主题不存在/);
});

test('移除插件会删除目录中的文件，删除主题会去掉波波目录', async (t) => {
  const config = fixture(t);
  config.pluginsRuntime = await loadConfiguredPlugins(config);
  const removed = await removePlugin(config, { id: 'alarm_clock' });
  assert.equal(removed.plugins.length, 0);
  assert.equal(fs.existsSync(path.join(config.configDirectory, 'plugins', 'alarm.mjs')), false);
  const themes = removeTheme(config, 'bobo');
  assert.deepEqual(themes, []);
  assert.equal(fs.existsSync(path.join(config.configDirectory, 'themes', 'bobo')), false);
});
