import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { loadConfiguredPlugins } from '../src/plugins/registry.mjs';
import { reloadConfiguredPlugins, watchPlugins } from '../src/plugins/hot-reload.mjs';
import { JobStore } from '../src/jobs.mjs';

function temporary(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-plugin-hot-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return directory;
}

function writePlugin(directory, label, mark) {
  fs.writeFileSync(path.join(directory, 'mark.mjs'), `export const mark = ${JSON.stringify(mark)};\n`);
  fs.writeFileSync(path.join(directory, 'tag.json'), `${JSON.stringify({ tag: mark })}\n`);
  fs.writeFileSync(path.join(directory, 'plugin.mjs'), `import { mark } from './mark.mjs';
import tag from './tag.json' with { type: 'json' };
export default {
  apiVersion: 1, kind: 'coding-agent', id: 'hot_agent', label: ${JSON.stringify(label)}, icon: 'rabbit',
  capabilities: { session: 'native', model: false, cancel: true, progress: false },
  probe: () => ({ available: true }),
  run: async () => ({ status: 'completed', result: mark + ':' + tag.tag }),
};
`);
}

function configFor(directory) {
  return {
    configDirectory: directory,
    madeHome: path.join(directory, 'home'),
    plugins: ['./plugin.mjs'],
    disabledPlugins: [],
    defaultProvider: 'hot_agent',
    projects: [{ id: 'demo', name: 'demo', path: directory }],
    defaultProject: 'demo',
  };
}

test('插件热加载读到新模块，失败时保留上一份', async (t) => {
  const directory = temporary(t);
  fs.mkdirSync(path.join(directory, 'home'), { recursive: true });
  writePlugin(directory, '第一版', 'one');
  const config = configFor(directory);
  config.pluginsRuntime = await loadConfiguredPlugins(config);
  const store = new JobStore({
    file: path.join(directory, 'state', 'jobs.json'),
    projects: config.projects,
    defaultProject: config.defaultProject,
    defaultProvider: config.defaultProvider,
    pluginsRuntime: () => config.pluginsRuntime,
    run: { agent() { return undefined; } },
    providerAvailable: () => ({ available: true }),
  });
  assert.equal(config.pluginsRuntime.agent('hot_agent').label, '第一版');
  assert.equal((await config.pluginsRuntime.agent('hot_agent').run()).result, 'one:one');

  writePlugin(directory, '第二版', 'two');
  await reloadConfiguredPlugins(config);
  assert.equal(store.agent('hot_agent').label, '第二版');
  assert.equal((await store.agent('hot_agent').run()).result, 'two:two');

  fs.writeFileSync(path.join(directory, 'plugin.mjs'), 'export default { apiVersion: 1 };\n');
  await assert.rejects(() => reloadConfiguredPlugins(config));
  assert.equal(store.agent('hot_agent').label, '第二版');
  assert.equal((await store.agent('hot_agent').run()).result, 'two:two');
});

test('保存插件文件会自动重新加载', async (t) => {
  const directory = temporary(t);
  fs.mkdirSync(path.join(directory, 'home'), { recursive: true });
  writePlugin(directory, '第一版', 'one');
  const config = configFor(directory);
  config.pluginsRuntime = await loadConfiguredPlugins(config);
  let result = undefined;
  const watch = watchPlugins(config, { onReload(error) { result = error || null; } });
  t.after(() => watch.close());
  // 监视句柄在 macOS 上不是同步就绪的，先让出事件循环再写文件。
  await new Promise((resolve) => setTimeout(resolve, 100));
  writePlugin(directory, '第三版', 'three');
  const started = Date.now();
  while (result === undefined && Date.now() - started < 4000) {
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  assert.equal(result, null);
  assert.equal(config.pluginsRuntime.agent('hot_agent').label, '第三版');
  assert.equal((await config.pluginsRuntime.agent('hot_agent').run()).result, 'three:three');
});
