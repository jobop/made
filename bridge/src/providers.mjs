import { normalizeAgentIcon } from './agent-icons.mjs';
import { translateKnownMessage } from './i18n.mjs';

export function agentFor(config, id) {
  return config.pluginsRuntime ? config.pluginsRuntime.agent(id) : undefined;
}

export function providerStates(config, locale = config.locale) {
  const agents = config.pluginsRuntime ? config.pluginsRuntime.agents() : [];
  return agents.map((plugin) => {
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
