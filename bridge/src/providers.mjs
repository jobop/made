import { builtinAgents, builtinAgent } from './agents/index.mjs';
import { normalizeAgentIcon } from './agent-icons.mjs';
import { translateKnownMessage } from './i18n.mjs';

export { builtinAgents } from './agents/index.mjs';

export function agentFor(config, id) {
  return config.pluginsRuntime ? config.pluginsRuntime.agent(id) : builtinAgent(id);
}

export function isBuiltinAgent(plugin) {
  const builtin = builtinAgent(plugin?.id);
  return Boolean(builtin && plugin.run === builtin.run && plugin.probe === builtin.probe);
}

export function providerStates(config, locale = config.locale) {
  const agents = config.pluginsRuntime ? config.pluginsRuntime.agents() : builtinAgents;
  return agents.map((plugin) => {
    let state;
    let bridgeReason = isBuiltinAgent(plugin);
    try {
      state = plugin.probe(config);
      if (state?.then) {
        // A mistaken async probe cannot hold up /api/state or produce an
        // unhandled rejection. The plugin contract requires a sync result.
        Promise.resolve(state).catch(() => {});
        bridgeReason = true;
        state = { available: false, reason: '插件状态检查必须同步返回' };
      }
    } catch { bridgeReason = true; state = { available: false, reason: `${plugin.label} 状态检查失败` }; }
    return {
      id: plugin.id, label: plugin.label, icon: normalizeAgentIcon(plugin.icon),
      available: state?.available === true,
      reason: typeof state?.reason === 'string' ? (bridgeReason ? translateKnownMessage(state.reason, locale) : state.reason).slice(0, 512) : '',
      mode: typeof state?.mode === 'string' ? state.mode.slice(0, 64) : 'plugin',
      capabilities: {
        session: plugin.capabilities.session, model: plugin.capabilities.model,
        cancel: plugin.capabilities.cancel, progress: plugin.capabilities.progress,
      },
      model: plugin.capabilities.model && plugin.model
        ? { default: plugin.model.default, required: plugin.model.required } : null,
      canCancelRunning: plugin.capabilities.cancel,
    };
  });
}

// Compatibility for existing stream tests and embedders. Actual parsing belongs
// to each plugin and adding an agent never adds another branch here.
export function createCliEventStream(provider, onProgress, onSession) {
  const plugin = builtinAgent(provider);
  if (!plugin?.createEvents) throw new Error(`编程工具不支持 CLI 事件：${provider}`);
  return plugin.createEvents(onProgress, onSession);
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
