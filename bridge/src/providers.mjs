import { normalizeAgentIcon } from './agent-icons.mjs';
import { translateKnownMessage } from './i18n.mjs';

export function agentFor(config, id) {
  return config.pluginsRuntime ? config.pluginsRuntime.agent(id) : undefined;
}

// 探针快照缓存：providerStates 每次都重跑全部插件 probe，而 CLI 探针内部是
// PATH 全量 statSync 扫描 + 60s TTL 的 spawnSync 登录检查。/api/state 是秒级
// 轮询端点，不缓存会把它放大成每秒上百次 stat、每分钟数次事件循环最长 3s 的
// 同步冻结。3s TTL 意味着设置变更最迟 3s 可见；调用方只读不写返回值。
// 按插件运行时实例隔离（WeakMap）：不同注册表（测试/重载）互不串味。
const PROVIDER_STATES_TTL_MS = 3000;
const providerStatesCaches = new WeakMap();

export function providerStates(config, locale = config.locale) {
  const runtime = config.pluginsRuntime;
  if (!runtime) return [];
  let byLocale = providerStatesCaches.get(runtime);
  if (!byLocale) {
    byLocale = new Map();
    providerStatesCaches.set(runtime, byLocale);
  }
  const cached = byLocale.get(locale);
  if (cached && cached.until > Date.now()) return cached.states;
  const agents = runtime.agents();
  const states = agents.map((plugin) => {
    let state;
    try {
      state = plugin.probe(config);
      if (state?.then) {
        // A mistaken async probe cannot hold up /api/state or produce an
        // unhandled rejection. The plugin contract requires a sync result.
        Promise.resolve(state).catch(() => {});
        state = { available: false, reason: '插件状态检查必须同步返回' };
      }
    } catch { state = { available: false, reason: `${plugin.label} 状态检查失败` }; }
    return {
      id: plugin.id, label: plugin.label, icon: normalizeAgentIcon(plugin.icon),
      available: state?.available === true,
      // 桥接器规范文案（bridgeStrings 插件）在输出边界翻译；第三方插件自有
      // 提示原样返回，即使碰巧撞上桥接器文案也不改写。
      reason: typeof state?.reason === 'string'
        ? (plugin.bridgeStrings === true ? translateKnownMessage(state.reason, locale) : state.reason).slice(0, 512)
        : '',
      mode: typeof state?.mode === 'string' ? state.mode.slice(0, 64) : 'plugin',
      capabilities: {
        session: plugin.capabilities.session, model: plugin.capabilities.model,
        cancel: plugin.capabilities.cancel, progress: plugin.capabilities.progress,
      },
      model: plugin.capabilities.model && plugin.model
        ? { default: plugin.model.default, required: plugin.model.required } : null,
      // 形象素材清单：码得按 version 决定是否分片重拉（docs/SPRITES.md）。
      sprite: plugin.sprite ? { version: plugin.sprite.version, files: plugin.sprite.files } : null,
      canCancelRunning: plugin.capabilities.cancel,
    };
  });
  providerStatesCaches.get(runtime).set(locale, { states, until: Date.now() + PROVIDER_STATES_TTL_MS });
  return states;
}

export function createRunner(config) {
  const runner = async (job, project, signal, onProgress = () => {}, onSession = () => {}) => {
    const plugin = agentFor(config, job.provider);
    if (!plugin) throw new Error('编程工具插件未加载，请先安装或启用对应插件');
    if (signal.aborted) throw new Error('任务已取消');
    // Capture the model when starting the turn; editing desktop settings does
    // not mutate an in-flight run. Model-less adapters never receive a model.
    const model = plugin.capabilities.model ? config.models?.[plugin.id] : undefined;
    return plugin.run({
      job, project, signal, config, model,
      onProgress: plugin.capabilities.progress ? onProgress : () => {},
      onSession: plugin.capabilities.session === 'native' ? onSession : () => {},
    });
  };
  runner.agent = (id) => agentFor(config, id);
  runner.hasAgent = (id) => Boolean(runner.agent(id));
  runner.canReadUpdates = (job) => typeof runner.agent(job.provider)?.readUpdates === 'function';
  runner.readUpdates = (job) => runner.agent(job.provider)?.readUpdates?.({ job, config });
  runner.syncJobState = (job) => runner.agent(job.provider)?.syncJobState?.(job);
  return runner;
}
