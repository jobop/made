// 云端 LLM 通道：云端码得（made-cloud）拉起的沙箱里注入的模型环境。
// 公开契约（cloud 注入沙箱环境变量，插件配置可用 ${...} 占位符引用，
// 见 plugin-package.mjs 的 resolveCloudPlaceholders）：
//   MADE_LLM_BASE_URL  云侧 OpenAI 兼容网关（如 https://cloud.example/llm）
//   MADE_LLM_TOKEN     沙箱令牌（scope=sandbox），网关鉴权用；停沙箱即吊销
//   MADE_LLM_MODEL     云侧指定的默认模型 ID（如 Qwen/Qwen3-32B）
// 兼容旧变量名 LLM_BASE_URL / LLM_TOKEN / LLM_DEFAULT_MODEL（未重建的沙箱）。
//
// 设计边界：插件配置字段的语义只有开发者知道，桥接器不做任何猜测——
// 插件想用云端通道，由开发者在 plugin.json 的字段 default 里显式写
// "${MADE_LLM_BASE_URL}" 等占位符；这里只负责桥接器自身的模型路由兜底。
export function cloudLlmEnv(env = process.env) {
  const baseUrl = String(env.MADE_LLM_BASE_URL || env.LLM_BASE_URL || '').trim().replace(/\/+$/, '');
  const token = String(env.MADE_LLM_TOKEN || env.LLM_TOKEN || '').trim();
  if (!baseUrl || !token) return null;
  let parsed;
  try { parsed = new URL(baseUrl); } catch { return null; }
  if (!['http:', 'https:'].includes(parsed.protocol) || parsed.username || parsed.password) return null;
  const model = String(env.MADE_LLM_MODEL || env.LLM_DEFAULT_MODEL || '').trim();
  return { baseUrl, token, model };
}

// 桥接器自身的模型路由兜底：某助手既没有用户/工作台设置的模型
// （config.models[provider]），插件配置里也解析不出任何非空模型串时，
// 用云端默认模型顶上。不读不写插件配置字段本身。
export function applyCloudLlmDefaults(config) {
  const cloud = cloudLlmEnv();
  if (!cloud?.model) return [];
  const runtime = config.pluginsRuntime;
  const agents = runtime ? runtime.agents() : [];
  const changed = [];
  for (const plugin of agents) {
    if (!plugin?.capabilities?.model) continue;
    if (String(config.models?.[plugin.id] || '').trim()) continue;
    const settings = config.pluginSettings?.[plugin.id] || {};
    const hasModel = Object.values(settings).some((v) => typeof v === 'string' && v.trim());
    if (hasModel) continue;
    config.models = { ...(config.models || {}), [plugin.id]: cloud.model };
    changed.push(plugin.id);
  }
  return changed;
}
