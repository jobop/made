import { builtinAgentIcons } from '../agent-icons.mjs';
import { translateKnownMessage } from '../i18n.mjs';
async function run({ job, config, signal }) {
  if (!config.workbuddy?.allowUnscopedDispatch || !config.workbuddyToken) {
    throw new Error('WorkBuddy 开放平台尚未授权或未开启转交模式');
  }
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
    result: config.locale === 'en'
      ? `Sent to WorkBuddy. Local message ID: ${data.data?.message_id || 'unknown'}. Continue in WorkBuddy for execution and permission requests.`
      : `已转交给 WorkBuddy，本机消息 ID：${data.data?.message_id || '未知'}。请在 WorkBuddy 中查看后续执行与权限请求。`,
    handoff: { cursor: data.data?.message_id || '', readState: 'retrying', replyCount: 0 },
  };
}

async function readMessages(job, config) {
  if (!config.workbuddyToken || !job.handoff?.cursor) return { messages: [] };
  const url = new URL('https://www.workbuddy.cn/openapi/v2/localassistant/message');
  url.searchParams.set('message_id', job.handoff?.cursor);
  const response = await fetch(url, {
    method: 'GET',
    signal: AbortSignal.timeout(15000),
    headers: {
      Authorization: `Bearer ${config.workbuddyToken}`,
      Accept: 'application/json',
    },
  });
  if (response.status === 401 || response.status === 403) {
    return { unreadable: true };
  }
  const data = await response.json();
  if (!response.ok || data.code !== 0) {
    throw new Error(`WorkBuddy 消息读取失败：${data.msg || response.status}`);
  }
  if (!Array.isArray(data.data?.messages)) {
    throw new Error('WorkBuddy 消息历史格式无效');
  }
  return {
    messages: data.data.messages.slice(0, 100).map((message) => ({
      id: typeof message.message_id === 'string' ? message.message_id : '',
      role: message.role,
      type: message.msg_type,
      text: Array.isArray(message.content)
        ? message.content.filter((part) => typeof part === 'string').join('\n').slice(0, 2000)
        : '',
    })),
  };
}

// Migration aliases are confined to this builtin adapter so existing state and
// older desktop clients remain compatible without shaping the plugin protocol.
function syncJobState(job) {
  if (!job.handoff && job.workbuddyCursor) {
    job.handoff = { cursor: job.workbuddyCursor, readState: job.workbuddyReadState || 'retrying', replyCount: job.workbuddyReplyCount || 0 };
  }
  if (!job.handoff) return;
  job.workbuddyMessageId ||= job.handoff.cursor || '';
  job.workbuddyCursor = job.handoff.cursor;
  job.workbuddyReadState = job.handoff.readState;
  job.workbuddyReplyCount = job.handoff.replyCount || 0;
  if (job.handoffReadError) job.workbuddyReadError = job.handoffReadError;
  else delete job.workbuddyReadError;
}

async function readUpdates({ job, config }) {
  const localize = (value) => translateKnownMessage(value, config.locale);
  const update = await readMessages(job, config);
  const handoff = { ...job.handoff };
  const additions = [];
  if (update.unreadable) {
    handoff.readState = 'unavailable';
    const notice = localize('当前授权不能读取 WorkBuddy 消息；需 user.localassistant.readable 权限。请在 WorkBuddy 中查看回复。');
    if (!job.result?.includes(notice)) additions.push(notice);
  } else {
    handoff.readState = 'readable';
    for (const message of update.messages || []) {
      if (!message.id || message.id === handoff.cursor) continue;
      // This API is a shared stream: another user prompt makes association
      // ambiguous, so later replies must not be attached to this handoff.
      if (message.role === 'user') {
        handoff.readState = 'ambiguous';
        additions.push(localize('出现其他 WorkBuddy 用户消息；后续回复无法可靠归属此任务，请在 WorkBuddy 中查看。'));
        break;
      }
      handoff.cursor = message.id;
      if (message.role !== 'assistant') continue;
      const reply = message.text || (message.type === 'permission'
        ? localize('助理请求进一步确认，请在 WorkBuddy 中处理。')
        : localize('助理有一条新消息，请在 WorkBuddy 中查看详情。'));
      additions.push(`${localize('WorkBuddy 后续助理消息（任务关联未验证）：')}\n${reply}`);
      handoff.replyCount = (handoff.replyCount || 0) + 1;
    }
  }
  return { handoff, appendResult: additions.join('\n\n') };
}

export default {
  apiVersion: 1, kind: 'coding-agent', id: 'workbuddy', label: 'WorkBuddy', icon: builtinAgentIcons.panda,
  capabilities: { session: 'external_unscoped', model: false, cancel: false, progress: false },
  probe(config) {
    const available = Boolean(config.workbuddyToken && config.workbuddy?.allowUnscopedDispatch);
    return { available, mode: 'dispatch_only', reason: available
      ? '可转交任务；如获消息读取授权可回显回复，无法保证指定代码目录或撤回'
      : '需 WorkBuddy 开放平台授权，并显式允许无项目目录保证的转交模式' };
  },
  run, readUpdates, syncJobState,
};
