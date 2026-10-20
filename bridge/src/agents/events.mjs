export function toolActivity(name, completed = false) {
  const tool = String(name || '').toLowerCase();
  if (/read|view|open/.test(tool)) return completed ? '文件读取完成' : '正在读取文件';
  if (/write|edit|patch|apply|delete/.test(tool)) return completed ? '文件修改完成' : '正在修改文件';
  if (/terminal|shell|bash|command|exec/.test(tool)) return completed ? '命令执行完成' : '正在运行命令';
  if (/search|grep|glob|find/.test(tool)) return completed ? '代码检索完成' : '正在检索代码';
  return completed ? '工具操作完成' : '正在使用工具';
}

export function textBlocks(content) {
  if (typeof content === 'string') return content;
  if (!Array.isArray(content)) return '';
  return content.filter((part) => part?.type === 'text' && typeof part.text === 'string')
    .map((part) => part.text).join('');
}

export function streamActivity(event) {
  if (event.type === 'system' && event.subtype === 'init') return '已连接编程工具';
  if (event.type === 'tool_call') {
    const name = Object.keys(event.tool_call || {})[0];
    return toolActivity(name, event.subtype === 'completed');
  }
  if (event.type === 'stream_event') {
    const inner = event.event;
    if (inner?.type === 'content_block_start' && inner.content_block?.type === 'tool_use') {
      return toolActivity(inner.content_block.name);
    }
    if (inner?.type === 'content_block_delta' && inner.delta?.type === 'text_delta') return '正在整理答复';
    return '';
  }
  if (event.type === 'assistant') {
    const toolBlock = event.message?.content?.find?.((part) => part?.type === 'tool_use');
    return toolBlock ? toolActivity(toolBlock.name) : '正在整理答复';
  }
  if (event.type === 'result' && event.subtype === 'success') return '正在整理结果';
  return '';
}

// 工具调用捕获：从 CLI 原始 JSONL 事件里抽 {name, detail, state}，只进桥接器
// 任务记录（网页端折叠展示），绝不混入设备进度文本。codex 的 item.* 事件与
// Claude 风格的 tool_call/assistant 事件都覆盖；认不出的形状安静返回 null。
const TOOL_DETAIL_LIMIT = 600;

function truncate(value, limit = TOOL_DETAIL_LIMIT) {
  const text = String(value ?? '').trim();
  return text.length <= limit ? text : `${text.slice(0, limit)}…（截断）`;
}

function toolDetail(parts) {
  return truncate(parts.filter((part) => String(part || '').trim()).join('  '));
}

export function toolCallFromEvent(event) {
  if (event.type === 'tool_call' && event.tool_call && typeof event.tool_call === 'object') {
    const name = Object.keys(event.tool_call)[0] || '';
    const args = event.tool_call[name];
    return {
      name: truncate(name, 120),
      detail: toolDetail([name, typeof args === 'string' ? args : JSON.stringify(args)]),
      state: event.subtype === 'completed' ? 'completed' : 'started',
      key: '',
    };
  }
  if (event.type === 'stream_event' && event.event?.type === 'content_block_start' &&
      event.event.content_block?.type === 'tool_use') {
    return {
      name: truncate(event.event.content_block.name, 120),
      detail: toolDetail([event.event.content_block.name,
        JSON.stringify(event.event.content_block.input ?? '')]),
      state: 'started', key: String(event.event.index ?? ''),
    };
  }
  if (event.type === 'assistant' && Array.isArray(event.message?.content)) {
    const block = event.message.content.find((part) => part?.type === 'tool_use');
    if (!block) return null;
    return {
      name: truncate(block.name, 120),
      detail: toolDetail([block.name, JSON.stringify(block.input ?? '')]),
      state: 'started', key: String(block.id ?? ''),
    };
  }
  if (event.type !== 'item.started' && event.type !== 'item.completed') return null;
  const item = event.item;
  if (!item || typeof item !== 'object') return null;
  const completed = event.type === 'item.completed';
  switch (item.type) {
    case 'command_execution':
      return { name: '命令执行', detail: toolDetail([item.command, item.cwd]), state: completed ? 'completed' : 'started', key: String(item.id ?? '') };
    case 'file_change':
      return {
        name: '文件修改',
        detail: toolDetail(Array.isArray(item.changes)
          ? item.changes.map((change) => `${change?.path || ''}${change?.kind ? ` (${change.kind})` : ''}`)
          : []),
        state: completed ? 'completed' : 'started', key: String(item.id ?? ''),
      };
    case 'mcp_tool_call': case 'collab_tool_call':
      return { name: truncate(item.tool || '工具调用', 120), detail: toolDetail([item.tool, JSON.stringify(item.arguments ?? '')]), state: completed ? 'completed' : 'started', key: String(item.id ?? '') };
    case 'web_search':
      return { name: '网页搜索', detail: toolDetail([item.query]), state: completed ? 'completed' : 'started', key: String(item.id ?? '') };
    case 'reasoning': case 'agent_message': case 'todo_list': case 'error':
      return null;
    default:
      return null;
  }
}

// Only categorical activity is emitted by builtin adapters; raw tool arguments
// and reasoning never become the device progress text. Tool calls are drained
// separately (takeToolCalls) for the bridge UI only.
export function jsonLineEvents(interpret, onProgress = () => {}, onSession = () => {}) {
  let pending = '';
  let rawTail = '';
  let discardingOversized = false;
  let sawEvent = false;
  const state = { lastMessage: '', assistantDeltas: '', finalResult: '', terminalError: '' };
  // 半开工具调用：key → 在 calls 里的引用。completed 事件来时把对应条目置为
  // 完成；对不上号（CLI 没给 id 或乱序）就当作独立条目追加，宁可多一行。
  const open = new Map();
  const calls = [];
  const record = (call) => {
    if (!call || !call.name) return;
    if (call.state === 'completed' && call.key && open.has(call.key)) {
      const entry = open.get(call.key);
      entry.state = 'completed';
      if (call.detail && call.detail.length > entry.detail.length) entry.detail = call.detail;
      open.delete(call.key);
      return;
    }
    const entry = { name: call.name, detail: call.detail || '', state: call.state, at: new Date().toISOString() };
    calls.push(entry);
    if (calls.length > 80) calls.splice(0, calls.length - 80);
    if (entry.state === 'started' && call.key) open.set(call.key, entry);
  };
  const consume = (line) => {
    let event;
    try { event = JSON.parse(line); } catch { return; }
    if (!event || typeof event !== 'object' || typeof event.type !== 'string') return;
    sawEvent = true;
    record(toolCallFromEvent(event));
    const update = interpret(event, state) || {};
    if (typeof update.sessionId === 'string') onSession(update.sessionId);
    if (update.activity) onProgress(update.activity);
  };
  return {
    write(chunk) {
      const text = String(chunk);
      rawTail = `${rawTail}${text}`.slice(-8000);
      pending += text;
      let newline;
      while ((newline = pending.indexOf('\n')) !== -1) {
        const line = pending.slice(0, newline);
        pending = pending.slice(newline + 1);
        if (!discardingOversized && line.length <= 1_000_000) consume(line);
        discardingOversized = false;
      }
      if (pending.length > 1_000_000) { pending = ''; discardingOversized = true; }
    },
    finish() {
      if (!discardingOversized && pending.trim()) consume(pending);
      for (const entry of open.values()) entry.state = 'completed';
      open.clear();
      return {
        result: (state.finalResult || state.lastMessage || (sawEvent ? '任务已执行，未返回文字摘要。' : rawTail.trim())).slice(-8000),
        hasAssistantText: Boolean(state.finalResult || state.lastMessage || !sawEvent),
        error: state.terminalError, rawTail,
      };
    },
    takeToolCalls() {
      return calls.splice(0, calls.length);
    },
  };
}

export function assistantEvent(event, state, { append = false } = {}) {
  if (event.type === 'assistant') {
    const text = textBlocks(event.message?.content);
    if (text) {
      state.assistantDeltas = append ? `${state.assistantDeltas}${text}`.slice(-8000) : text;
      state.lastMessage = state.assistantDeltas;
    }
  } else if (event.type === 'result') {
    if (typeof event.result === 'string') state.finalResult = event.result;
    if (event.is_error || (event.subtype && event.subtype !== 'success')) state.terminalError = '编程工具报告任务失败';
    // 终局事件以最后 outcome 为准：部分 CLI 会先吐可恢复错误（如流断开重试）、
    // 随后仍以 success 收尾（实测 2026-10-16 小鱼任务：codex 自身 task_complete，
    // 桥接器却因早前的 error 事件判死）。先失败后成功 = 成功；只看首个终态会假失败。
    else if (event.subtype === 'success' || event.is_error === false) state.terminalError = '';
  } else if (event.type === 'turn.failed') state.terminalError = '编程工具报告任务失败';
}
