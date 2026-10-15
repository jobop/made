// 测试夹具：内置编程助手已插件化为市场包，但桥接器测试仍需要真实走一遍
// runCli（权限边界、看门狗、改动清单）与事件解析。这里按市场插件的同款
// 适配器逻辑构造 codex/cursor/qoder/workbuddy 夹具，机制直接用桥接器
// src/agents/ 里的实现，保证被测的就是线上机制本身。
import { runCli } from '../../src/agents/shared.mjs';
import { jsonLineEvents, assistantEvent, streamActivity, toolActivity } from '../../src/agents/events.mjs';
import { createPluginRegistry } from '../../src/plugins/registry.mjs';
import { builtinSpeechRecognizers } from '../../src/speech/index.mjs';

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

/** 测试用的插件注册表：四个夹具助手 + 内置语音识别。 */
export function testRegistry() {
  return createPluginRegistry([codexFixture, cursorFixture, qoderFixture, workbuddyFixture, ...builtinSpeechRecognizers]);
}

function cliAgent({ id, label, icon, command, classifyExit, probe, model }) {
  const createEvents = (onProgress, onSession) => jsonLineEvents(
    id === 'codex' ? (event, state) => {
          const sessionId = event.type === 'thread.started' ? event.thread_id : undefined;
          if (event.item?.type === 'agent_message' && event.type === 'item.completed' && typeof event.item.text === 'string') state.lastMessage = event.item.text;
          if (event.type === 'turn.failed' || event.type === 'error') state.terminalError = '编程工具报告任务失败';
          let activity = '';
          if (event.type === 'turn.started') activity = '正在分析任务';
          if (event.type === 'turn.completed') activity = '正在整理结果';
          if (event.type.startsWith('item.')) {
            const completed = event.type === 'item.completed';
            switch (event.item?.type) {
              case 'command_execution': activity = toolActivity('command', completed); break;
              case 'file_change': activity = toolActivity('edit', completed); break;
              case 'web_search': activity = completed ? '资料检索完成' : '正在查找资料'; break;
              case 'mcp_tool_call': case 'collab_tool_call': activity = toolActivity('', completed); break;
              case 'todo_list': if (completed) activity = '已更新任务步骤'; break;
              case 'agent_message': if (completed) activity = '正在整理答复'; break;
            }
          }
          return { sessionId, activity };
        } : (event, state) => {
          assistantEvent(event, state, { append: id === 'cursor' });
          return {
            sessionId: id === 'cursor'
              ? event.session_id
              : (event.type === 'system' && event.subtype === 'init' ? event.session_id || event.data?.session_id : undefined),
            activity: streamActivity(event),
          };
        },
    onProgress, onSession,
  );
  return {
    apiVersion: 1, kind: 'coding-agent', id, label, icon,
    capabilities: { session: 'native', model: true, cancel: true, progress: true },
    bridgeStrings: true,
    ...(model ? { model } : {}),
    ...(probe ? { probe } : { probe: () => ({ available: true, mode: 'local_cli', reason: '' }) }),
    createEvents,
    run: (context) => runCli(context, {
      label,
      createEvents,
      command,
      ...(classifyExit ? { classifyExit } : {}),
    }),
  };
}

export const codexFixture = cliAgent({
  id: 'codex', label: 'Codex', icon: 'fox', model: { default: 'gpt-6-astra', required: true },
  command({ cwd, instruction, config, model, sessionId }) {
    const args = ['exec', '--json', '--sandbox', 'workspace-write', '-C', cwd];
    if (sessionId) args.push('resume');
    const selected = model ?? config.codexModel;
    if (selected) args.push('-m', selected);
    if (sessionId) args.push(sessionId);
    args.push('-');
    return { command: 'codex', args, stdin: instruction };
  },
  classifyExit({ job, project, stderr, output, startingGitSnapshot }) {
    const activeWriter = /Failed to create session:\s*thread-store conflict:\s*thread [A-Za-z0-9_-]{6,128} already has an active writer/;
    if (job.sessionId && activeWriter.test(`${stderr}\n${output.rawTail}`)) {
      const error = new Error('此 Codex 对话正在另一窗口或进程使用；结束后再次确认即可继续');
      error.code = 'CODEX_SESSION_ACTIVE_WRITER';
      error.retryable = true;
      return error;
    }
    if (/model is not supported when using Codex|Model metadata for/.test(output.rawTail)) {
      return new Error('Codex 当前模型不能用于这个登录账号。请在电脑桥接页面的“编程助手模型”中选择该账号可用的模型。');
    }
    void job; void project; void startingGitSnapshot;
  },
  probe: () => ({ available: true, mode: 'local_cli', reason: '' }),
});

export const cursorFixture = cliAgent({
  id: 'cursor', label: 'Cursor', icon: 'rabbit',
  command({ cwd, instruction, model, sessionId }) {
    const args = ['--print', '--trust', '--output-format', 'stream-json', '--stream-partial-output', '--sandbox', 'enabled', '--auto-review', '--workspace', cwd];
    if (sessionId) args.push('--resume', sessionId);
    if (model) args.push('--model', model);
    args.push(instruction);
    return { command: 'cursor-agent', args };
  },
});

export const qoderFixture = cliAgent({
  id: 'qoder', label: 'Qoder', icon: 'owl',
  command({ cwd, instruction, model, sessionId }) {
    const args = ['--print', '--output-format', 'stream-json', '--permission-mode', 'accept_edits', '--cwd', cwd];
    if (sessionId) args.push('--session-id', sessionId);
    if (model) args.push('--model', model);
    args.push(instruction);
    return { command: 'qoder', args };
  },
});

export const workbuddyFixture = {
  apiVersion: 1, kind: 'coding-agent', id: 'workbuddy', label: 'WorkBuddy', icon: 'panda',
  capabilities: { session: 'external_unscoped', model: false, cancel: false, progress: false },
  bridgeStrings: true,
  probe(config) {
    const available = Boolean(config.workbuddyToken && config.workbuddy?.allowUnscopedDispatch);
    return { available, mode: 'dispatch_only', reason: available ? '' : '需 WorkBuddy 开放平台授权，并显式允许无项目目录保证的转交模式' };
  },
  async run({ job, config, signal }) {
    const response = await fetch('https://www.workbuddy.cn/openapi/v2/localassistant/message', {
      method: 'POST',
      signal: AbortSignal.any([signal, AbortSignal.timeout(30000)]),
      headers: {
        Authorization: `Bearer ${config.workbuddyToken}`,
        'Content-Type': 'application/json',
        Accept: 'application/json',
      },
      body: JSON.stringify({ content: job.instruction, msg_type: 'text' }),
    });
    const data = await response.json();
    if (!response.ok || data.code !== 0) {
      throw new Error(`WorkBuddy 转交失败：${data.msg || response.status}`);
    }
    return {
      status: 'handed_off',
      result: `已转交给 WorkBuddy，本机消息 ID：${data.data?.message_id || '未知'}。`,
      handoff: { cursor: data.data?.message_id || '', readState: 'retrying', replyCount: 0 },
    };
  },
  syncJobState(job) {
    if (!job.handoff && job.workbuddyCursor) {
      job.handoff = { cursor: job.workbuddyCursor, readState: job.workbuddyReadState || 'retrying', replyCount: job.workbuddyReplyCount || 0 };
    }
    if (!job.handoff) return;
    job.workbuddyMessageId ||= job.handoff.cursor || '';
    job.workbuddyCursor = job.handoff.cursor;
    job.workbuddyReadState = job.handoff.readState;
    job.workbuddyReplyCount = job.handoff.replyCount || 0;
  },
  async readUpdates({ job, config }) {
    if (!config.workbuddyToken || !job.handoff?.cursor) return { handoff: { ...job.handoff }, appendResult: '' };
    const url = new URL('https://www.workbuddy.cn/openapi/v2/localassistant/message');
    url.searchParams.set('message_id', job.handoff?.cursor);
    const response = await fetch(url, {
      method: 'GET',
      signal: AbortSignal.timeout(15000),
      headers: { Authorization: `Bearer ${config.workbuddyToken}`, Accept: 'application/json' },
    });
    if (response.status === 401 || response.status === 403) {
      const notice = '当前授权不能读取 WorkBuddy 消息；需 user.localassistant.readable 权限。请在 WorkBuddy 中查看回复。';
      return {
        handoff: { ...job.handoff, readState: 'unavailable' },
        appendResult: job.result?.includes(notice) ? '' : notice,
      };
    }
    const data = await response.json();
    if (!response.ok || data.code !== 0) {
      throw new Error(`WorkBuddy 消息读取失败：${data.msg || response.status}`);
    }
    const handoff = { ...job.handoff, readState: 'readable' };
    const additions = [];
    for (const message of data.data?.messages || []) {
      if (!message.message_id || message.message_id === handoff.cursor) continue;
      if (message.role === 'user') {
        handoff.readState = 'ambiguous';
        break;
      }
      handoff.cursor = message.message_id;
      if (message.role !== 'assistant') continue;
      additions.push((Array.isArray(message.content) ? message.content.filter((p) => typeof p === 'string').join('\n') : ''));
      handoff.replyCount = (handoff.replyCount || 0) + 1;
    }
    return { handoff, appendResult: additions.join('\n\n') };
  },
};
