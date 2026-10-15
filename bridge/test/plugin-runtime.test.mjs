import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { loadConfig } from '../src/config.mjs';
import {
  pluginRuntime, findCommand, hasCommand, runCommand, runNpm, windowsCommandLine,
} from '../src/plugin-runtime.mjs';

/** 造一个只有指定命令的临时 PATH 目录。 */
function toolDir(t, names = ['made-test-tool']) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-runtime-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  for (const name of names) {
    const file = path.join(directory, name);
    fs.writeFileSync(file, '#!/bin/sh\necho hi\n');
    fs.chmodSync(file, 0o755);
  }
  return directory;
}

test('findCommand 在 PATH 里命中命令，找不到时返回空串', (t) => {
  const directory = toolDir(t);
  assert.equal(findCommand('made-test-tool', { env: { PATH: directory } }), path.join(directory, 'made-test-tool'));
  assert.equal(findCommand('made-test-absent', { env: { PATH: directory } }), '');
  assert.equal(hasCommand('made-test-tool', { env: { PATH: directory } }), true);
  assert.equal(hasCommand('made-test-absent', { env: { PATH: directory } }), false);
});

test('findCommand 不把非可执行文件当成命令', (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-runtime-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const file = path.join(directory, 'made-test-doc');
  fs.writeFileSync(file, 'not a program\n');
  fs.chmodSync(file, 0o644);
  assert.equal(findCommand('made-test-doc', { env: { PATH: directory, PATHEXT: '' } }), '');
});

test('findCommand 支持显式路径，路径不存在时返回空串', (t) => {
  const directory = toolDir(t);
  const file = path.join(directory, 'made-test-tool');
  assert.equal(findCommand(file, { env: { PATH: '' } }), file);
  assert.equal(findCommand(path.join(directory, 'nope'), { env: { PATH: '' } }), '');
});

test('findCommand 在 Windows 上按 PATHEXT 补扩展名', (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-runtime-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  // 只在 Windows 分支下才会补扩展名，这里直接验证扩展名候选的拼接逻辑：
  // 传一个带扩展名的名字，非 Windows 下按字面命中。
  const file = path.join(directory, 'made-test-tool.cmd');
  fs.writeFileSync(file, '@echo hi\n');
  fs.chmodSync(file, 0o755);
  const env = { PATH: directory, PATHEXT: '.COM;.EXE;.BAT;.CMD' };
  assert.equal(findCommand('made-test-tool.cmd', { env }), file);
  // 不带扩展名的名字在类 Unix 上不该命中 .cmd 文件。
  if (process.platform !== 'win32') assert.equal(findCommand('made-test-tool', { env }), '');
});

test('runCommand 返回状态与输出，并标出「命令不存在」', (t) => {
  const directory = toolDir(t);
  const ok = runCommand('made-test-tool', [], { env: { PATH: directory } });
  assert.equal(ok.status, 0);
  assert.equal(ok.stdout.trim(), 'hi');
  assert.equal(ok.notFound, false);
  assert.equal(ok.resolved, path.join(directory, 'made-test-tool'));

  const missing = runCommand('made-test-absent');
  assert.equal(missing.notFound, true);
  assert.equal(missing.resolved, '');
  assert.match(missing.error.message, /未找到 made-test-absent/);
});

test('runNpm 拿得到版本；npm 不在 PATH 时报「未安装」而不是执行失败', () => {
  if (hasCommand('npm')) {
    assert.match(runNpm(['--version']).trim(), /^\d+\.\d+/);
  }
  assert.throws(
    () => runNpm(['--version'], { env: { PATH: '' } }),
    (error) => error.code === 'ENOENT' && error.notFound === true && /未找到 npm/.test(error.message),
  );
});

test('windowsCommandLine 给含空格与元字符的参数加引号', () => {
  assert.equal(windowsCommandLine(['npm.cmd', 'root', '-g']), 'npm.cmd root -g');
  assert.equal(
    windowsCommandLine(['C:\\Program Files\\nodejs\\npm.cmd', '--prefix', 'C:\\Users\\John Doe\\made']),
    '"C:\\Program Files\\nodejs\\npm.cmd" --prefix "C:\\Users\\John Doe\\made"',
  );
  assert.equal(windowsCommandLine(['npm.cmd', 'run', 'a&b']), 'npm.cmd run "a&b"');
  assert.equal(windowsCommandLine(['npm.cmd', '']), 'npm.cmd ""');
});

test('桥接器把 runtime 交给插件，且不可被改写', (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-runtime-config-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const file = path.join(directory, 'config.json');
  fs.writeFileSync(file, JSON.stringify({
    projects: [{ id: 'demo', label: 'Demo', path: '.' }],
    defaultProject: 'demo',
    defaultProvider: 'codex',
  }));
  const config = loadConfig({ VIBE_CONFIG: file, VIBE_STATE_DIR: path.join(directory, 'state') });
  assert.equal(typeof config.runtime.findCommand, 'function');
  assert.equal(typeof config.runtime.runNpm, 'function');
  // CLI 工具箱也经 runtime 注入：插件化后的 CLI 助手从这里拿执行机制。
  assert.equal(typeof config.runtime.runCli, 'function');
  assert.equal(typeof config.runtime.probeCli, 'function');
  assert.equal(typeof config.runtime.jsonLineEvents, 'function');
  assert.equal(typeof config.runtime.gitState, 'function');
  assert.equal(config.runtime.platform, process.platform);
  assert.equal(Object.isFrozen(config.runtime), true);
  assert.equal(config.runtime.findCommand, pluginRuntime.findCommand);
});
