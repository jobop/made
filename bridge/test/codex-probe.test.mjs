import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import codex from '../src/agents/codex.mjs';

test('codex 探测：未登录但配置了自定义 provider 鉴权时应视为可用', (t) => {
  const realHome = process.env.HOME;
  const home = fs.mkdtempSync(path.join(os.tmpdir(), 'codex-home-'));
  process.env.HOME = home;
  t.after(() => {
    process.env.HOME = realHome;
    delete process.env.MY_TEST_KEY;
    fs.rmSync(home, { recursive: true, force: true });
  });

  // 没有 ~/.codex/config.toml 时，探测结果应保持“未登录不可用”。
  assert.equal(codex.probe().available, false);

  fs.mkdirSync(path.join(home, '.codex'));
  // experimental_bearer_token 直接写在配置里，无需 ChatGPT 登录即可用。
  fs.writeFileSync(path.join(home, '.codex', 'config.toml'),
    'model_provider = "custom"\n\n[model_providers.custom]\nwire_api = "responses"\nrequires_openai_auth = false\nexperimental_bearer_token = "sk-test"\n');
  const state = codex.probe();
  assert.equal(state.available, true);
  assert.equal(state.mode, 'local_cli');

  // env_key 方式：环境变量不存在则不可用，存在才可用。
  fs.writeFileSync(path.join(home, '.codex', 'config.toml'),
    'model_provider = "custom"\n\n[model_providers.custom]\nenv_key = "MY_TEST_KEY"\n');
  assert.equal(codex.probe().available, false);
  process.env.MY_TEST_KEY = 'sk-test';
  assert.equal(codex.probe().available, true);
});
