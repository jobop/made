// 云端 LLM 通道测试：环境解析、桥接器自身模型路由兜底、云端语音识别插件。
// 字段语义不猜（契约见 docs/PLUGINS.md）：插件配置通过 ${MADE_LLM_*} 占位符
// 显式引用云端资源，解析逻辑在 plugin-package.test.mjs 覆盖。
// 运行：cd made/bridge && node --test test/cloud-llm.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';

process.env.LLM_BASE_URL = 'https://cloud.test/llm';
process.env.LLM_TOKEN = 'sandbox-token';
process.env.LLM_DEFAULT_MODEL = 'Qwen/Qwen3-32B';

const { cloudLlmEnv, applyCloudLlmDefaults } = await import('../src/cloud-llm.mjs');
const cloudRecognizer = (await import('../src/speech/cloud.mjs')).default;

test('cloudLlmEnv：齐全返回配置，缺项/非法 URL 返回 null', () => {
  assert.deepEqual(cloudLlmEnv(), { baseUrl: 'https://cloud.test/llm', token: 'sandbox-token', model: 'Qwen/Qwen3-32B' });
  // 新契约变量名优先于旧名
  const saved = process.env.LLM_BASE_URL;
  process.env.MADE_LLM_BASE_URL = 'https://new.test/llm';
  assert.equal(cloudLlmEnv().baseUrl, 'https://new.test/llm');
  delete process.env.MADE_LLM_BASE_URL;
  process.env.LLM_BASE_URL = saved;
  for (const key of ['LLM_BASE_URL', 'LLM_TOKEN']) {
    const old = process.env[key];
    delete process.env[key];
    assert.equal(cloudLlmEnv(), null, `缺 ${key} 应为 null`);
    process.env[key] = old;
  }
  const old = process.env.LLM_BASE_URL;
  process.env.LLM_BASE_URL = 'not a url';
  assert.equal(cloudLlmEnv(), null);
  process.env.LLM_BASE_URL = 'ftp://cloud.test/llm';
  assert.equal(cloudLlmEnv(), null);
  process.env.LLM_BASE_URL = old;
});

function fakeConfig({ settings = {}, models = {}, agents = [] } = {}) {
  return {
    pluginSettings: settings,
    models,
    pluginsRuntime: { agents: () => agents },
  };
}

// 桥接器自身模型路由兜底：不碰插件字段，只在「助手页没配模型、插件配置里
// 也解析不出任何非空串」时用云端默认模型顶上。
test('applyCloudLlmDefaults：无任何模型串时兜底 config.models；有则不动', () => {
  const agent = { id: 'mimi', capabilities: { model: true } };
  const empty = fakeConfig({ agents: [agent] });
  assert.deepEqual(applyCloudLlmDefaults(empty), ['mimi']);
  assert.equal(empty.models.mimi, 'Qwen/Qwen3-32B');
  assert.deepEqual(empty.pluginSettings, {}, '不写插件配置字段');

  // 插件配置里已有非空串（占位符解析出的云端值或用户自填值）→ 不兜底
  const filled = fakeConfig({ agents: [agent], settings: { mimi: { model: 'Qwen/Qwen3-32B' } } });
  assert.deepEqual(applyCloudLlmDefaults(filled), []);
  assert.equal(filled.models.mimi, undefined);

  // 用户/工作台已配助手页模型 → 不兜底
  const routed = fakeConfig({ agents: [agent], models: { mimi: 'my-model' } });
  assert.deepEqual(applyCloudLlmDefaults(routed), []);
  assert.equal(routed.models.mimi, 'my-model');
});

test('applyCloudLlmDefaults：无云端环境或无默认模型时不动任何配置', () => {
  const saved = process.env.LLM_DEFAULT_MODEL;
  delete process.env.LLM_DEFAULT_MODEL;
  try {
    const agent = { id: 'mimi', capabilities: { model: true } };
    const config = fakeConfig({ agents: [agent] });
    assert.deepEqual(applyCloudLlmDefaults(config), []);
    assert.deepEqual(config.models, {});
  } finally {
    process.env.LLM_DEFAULT_MODEL = saved;
  }
});

test('非 model 能力的 agent 不注入', () => {
  const config = fakeConfig({ agents: [{ id: 'plain', capabilities: {} }] });
  assert.deepEqual(applyCloudLlmDefaults(config), []);
  assert.deepEqual(config.models, {});
});

test('cloud 语音识别：透传音频与沙箱令牌，返回去抖后的文字', async (t) => {
  const wav = Buffer.alloc(1000);
  let captured;
  const realFetch = globalThis.fetch;
  globalThis.fetch = async (url, opts) => {
    captured = { url, opts };
    return new Response(JSON.stringify({ text: ' 你好云朵 ' }), { status: 200 });
  };
  t.after(() => { globalThis.fetch = realFetch; });
  const text = await cloudRecognizer.transcribe({ wav, signal: AbortSignal.timeout(5000) });
  assert.equal(text, '你好云朵');
  assert.equal(captured.url, 'https://cloud.test/llm/audio/transcriptions');
  assert.equal(captured.opts.headers.Authorization, 'Bearer sandbox-token');
  assert.ok(captured.opts.body instanceof FormData);
});

test('cloud 语音识别：上游错误带出错误信息；无环境时不可用', async (t) => {
  const realFetch = globalThis.fetch;
  globalThis.fetch = async () => new Response(JSON.stringify({ error: '额度不足' }), { status: 402 });
  t.after(() => { globalThis.fetch = realFetch; });
  await assert.rejects(
    cloudRecognizer.transcribe({ wav: Buffer.alloc(1000), signal: AbortSignal.timeout(5000) }),
    /额度不足/,
  );
  const saved = process.env.LLM_BASE_URL;
  delete process.env.LLM_BASE_URL;
  try {
    assert.equal(cloudRecognizer.probe().available, false);
    await assert.rejects(
      cloudRecognizer.transcribe({ wav: Buffer.alloc(1000), signal: AbortSignal.timeout(5000) }),
      /不可用/,
    );
  } finally {
    process.env.LLM_BASE_URL = saved;
  }
});
