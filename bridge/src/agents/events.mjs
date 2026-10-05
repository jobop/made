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

// Only categorical activity is emitted by builtin adapters; raw tool arguments
// and reasoning never become the device progress text.
export function jsonLineEvents(interpret, onProgress = () => {}, onSession = () => {}) {
  let pending = '';
  let rawTail = '';
  let discardingOversized = false;
  let sawEvent = false;
  const state = { lastMessage: '', assistantDeltas: '', finalResult: '', terminalError: '' };
  const consume = (line) => {
    let event;
    try { event = JSON.parse(line); } catch { return; }
    if (!event || typeof event !== 'object' || typeof event.type !== 'string') return;
    sawEvent = true;
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
      return {
        result: (state.finalResult || state.lastMessage || (sawEvent ? '任务已执行，未返回文字摘要。' : rawTail.trim())).slice(-8000),
        hasAssistantText: Boolean(state.finalResult || state.lastMessage || !sawEvent),
        error: state.terminalError, rawTail,
      };
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
  } else if (event.type === 'turn.failed') state.terminalError = '编程工具报告任务失败';
}
