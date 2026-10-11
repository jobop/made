/**
 * 增强运行时：桥接器向管控插件开放的接口面。
 *
 * 管控插件是"有自主行为能力的增强体"：触发（事件、回合钩子、任务通知）只决定它
 * 何时被唤醒；被唤醒后能做什么，取决于这里露出了多少句柄。接口有两条供给线：
 * - 板侧线：事件与命令，清单由固件上报，桥接器只做清单校验；要新命令（如关机）得改固件。
 * - 桥接器线：本模块露出的句柄（助手、任务、主题、板侧发送），重启桥接器即生效。
 *
 * 回合管道是顺序链：每个插件拿到上一环的产物，返回新字符串即 override，最后一个
 * 说话的算数。单个插件超时或出错时沿用上一环的值，绝不阻断任务本身。
 */

const HOOK_TIMEOUT_MS = 1500;
const CREATE_TASK_LIMIT = 10;
const CREATE_TASK_WINDOW_MS = 60_000;

export function createEnhancement(config) {
  const wired = { jobs: null, board: null, themes: null };
  const created = [];

  const boardPlugins = () => {
    try { return config.pluginsRuntime?.boardPlugins?.() ?? []; } catch { return []; }
  };

  const usable = (plugin) => {
    try {
      const state = plugin.probe(config);
      return Boolean(state && typeof state === 'object' && !(state instanceof Promise) && state.available === true);
    } catch { return false; }
  };

  async function settle(value, plugin) {
    let timer;
    try {
      return await Promise.race([
        Promise.resolve(value),
        new Promise((_, reject) => {
          timer = setTimeout(() => reject(new Error(`插件 ${plugin.id} 处理超时`)), HOOK_TIMEOUT_MS);
        }),
      ]);
    } finally {
      clearTimeout(timer);
    }
  }

  async function pipeline(hookName, initial, contextFor) {
    let value = initial;
    for (const plugin of boardPlugins()) {
      const hook = plugin[hookName];
      if (typeof hook !== 'function' || !usable(plugin)) continue;
      try {
        const next = await settle(hook(contextFor(value)), plugin);
        if (typeof next === 'string') value = next;
      } catch { /* 单个插件失败不阻断链，也不影响任务本身。 */ }
    }
    return value;
  }

  function agentView(plugin) {
    let state = {};
    try {
      const probed = plugin.probe(config);
      if (probed && typeof probed === 'object' && typeof probed.then !== 'function') state = probed;
    } catch { state = {}; }
    return {
      id: plugin.id,
      label: plugin.label,
      capabilities: plugin.capabilities,
      available: state.available === true,
      reason: typeof state.reason === 'string' ? state.reason : '',
      mode: typeof state.mode === 'string' ? state.mode : 'plugin',
    };
  }

  /** 桥接器句柄：插件在事件、钩子与通知的上下文里拿到的就是这个对象。 */
  function bridge() {
    const agents = () => {
      try { return (config.pluginsRuntime?.agents?.() ?? []).map(agentView); } catch { return []; }
    };
    return {
      agents,
      agent: (id) => agents().find((agent) => agent.id === id),
      jobs: () => (wired.jobs ? wired.jobs.list().map((job) => ({ ...job })) : []),
      job: (id) => {
        if (!wired.jobs) return undefined;
        const job = wired.jobs.get(id);
        return job ? { ...job } : undefined;
      },
      sessions: (agentId) => (wired.jobs ? wired.jobs.listSessions(agentId).map((session) => ({ ...session })) : []),
      createTask: ({ instruction, agentId, projectId, title } = {}) => {
        if (!wired.jobs) throw new Error('桥接器尚未就绪');
        createTaskGuard();
        const provider = agentId || config.defaultProvider;
        const project = projectId || wired.jobs.defaultProject;
        const session = wired.jobs.createSession({ provider, projectId: project, title });
        const job = wired.jobs.submit({ instruction, sessionId: session.id, provider, projectId: project, source: 'plugin' });
        return { job: { ...job }, session: { ...session } };
      },
      submitTurn: ({ instruction, sessionId } = {}) => {
        if (!wired.jobs || !sessionId) throw new Error('请指定要继续的任务');
        return { ...wired.jobs.submit({ instruction, sessionId, source: 'plugin' }) };
      },
      confirmJob: (id) => ({ ...wired.jobs.confirm(id) }),
      cancelJob: (id) => ({ ...wired.jobs.cancel(id) }),
      themes: () => (wired.themes ? wired.themes() : []),
      devices: () => (wired.board ? wired.board.deviceIds() : []),
      send: (deviceId, command, audio) => {
        if (!wired.board) throw new Error('桥接器尚未就绪');
        return wired.board.send(deviceId, command, audio);
      },
      pending: (deviceId) => (wired.board ? wired.board.pending(deviceId) : []),
    };
  }

  /** 回合输入管道：改写本轮实际执行的指令；存储里的任务原文保持不变。 */
  async function turnIn(job) {
    return pipeline('onTurnInput', job.instruction, (instruction) => ({
      instruction, job, agentId: job.provider, config, bridge: bridge(),
    }));
  }

  /** 回合输出管道：仅在任务完成时调用，返回新字符串即改写结果。 */
  async function turnOut(job, result) {
    return pipeline('onTurnOutput', result, (value) => ({
      result: value, job, agentId: job.provider, status: 'completed', config, bridge: bridge(),
    }));
  }

  /** 任务生命周期通知：created/confirmed/started/progress/completed/handed_off/failed/cancelled。 */
  function notify(type, job) {
    // 插件自己建的任务不再广播 created：否则"建任务→收到 created→再建任务"会自激。
    if (type === 'created' && job.source === 'plugin') return;
    for (const plugin of boardPlugins()) {
      if (typeof plugin.onJob !== 'function' || !usable(plugin)) continue;
      try {
        const result = plugin.onJob({ type, job, config, bridge: bridge() });
        if (result && typeof result.then === 'function') result.catch(() => {});
      } catch { /* 通知失败只影响这个插件自己。 */ }
    }
  }

  function createTaskGuard() {
    const at = Date.now();
    while (created.length && at - created[0] >= CREATE_TASK_WINDOW_MS) created.shift();
    if (created.length >= CREATE_TASK_LIMIT) throw new Error('插件创建任务过于频繁，请稍后再试');
    created.push(at);
  }

  function attach({ jobs, board, themes } = {}) {
    if (jobs) wired.jobs = jobs;
    if (board) wired.board = board;
    if (themes) wired.themes = themes;
  }

  return { attach, turnIn, turnOut, notify, bridge };
}
