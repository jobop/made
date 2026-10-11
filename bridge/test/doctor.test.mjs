import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { collectDoctor, formatDoctor } from '../src/doctor.mjs';

function fixture(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-doctor-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return {
    configDirectory: directory,
    madeHome: directory,
    configFile: path.join(directory, 'config.local.json'),
    stateDir: path.join(directory, 'state'),
    locale: 'zh-CN',
    plugins: [],
    disabledPlugins: [],
    pluginSettings: {},
    projects: [{ id: 'demo', label: 'Demo', path: directory }],
    defaultProject: 'demo',
    defaultProvider: 'codex',
    port: 8787,
    devicePort: 8788,
    models: {},
    usbPort: '',
  };
}

test('自检覆盖运行环境、依赖、接入方式与插件四块', async (t) => {
  const { checks, code } = await collectDoctor({ config: fixture(t) });
  const text = formatDoctor(checks);

  assert.match(text, /码得桥接器自检/);
  for (const section of ['运行环境', '依赖命令', '接入方式', '插件']) {
    assert.ok(text.includes(section), `缺少「${section}」分块`);
  }
  // 平台必须如实上报，规则才可能按平台给出不同结论。
  assert.ok(text.includes(process.platform), '自检里应带上当前平台');
  assert.equal(typeof code, 'number');
  // 有阻断项才给非零退出码，且必须与 fail 项一一对应。
  const fails = checks.filter((item) => item.level === 'fail').length;
  assert.equal(code, fails ? 1 : 0);
});

test('自检项渲染成三种记号，分块标题原样输出', () => {
  const text = formatDoctor([
    { raw: '分块标题' },
    { level: 'ok', label: '可用项' },
    { level: 'warn', label: '受限项', detail: '原因' },
    { level: 'fail', label: '缺项', detail: '怎么办' },
  ]);
  assert.equal(text, [
    '分块标题',
    '✓ 可用项',
    '! 受限项',
    '    原因',
    '✗ 缺项',
    '    怎么办',
  ].join('\n'));
});

test('默认助手缺失时算阻断项，退出码为 1', async (t) => {
  const config = { ...fixture(t), defaultProvider: 'not-installed' };
  const { checks, code } = await collectDoctor({ config });
  assert.equal(code, 1);
  assert.ok(
    checks.some((item) => item.level === 'fail' && item.label === '插件加载失败'),
    '默认助手不存在的插件加载错误应记为 fail',
  );
});
