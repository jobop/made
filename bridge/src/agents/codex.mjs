import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { builtinAgentIcons } from '../agent-icons.mjs';
import { probeCli, runCli, gitState } from './shared.mjs';
import { jsonLineEvents, toolActivity } from './events.mjs';

// `codex login status` 只认 ChatGPT/OAuth 登录。当用户在 ~/.codex/config.toml 里配置了
// 自定义 model_provider（experimental_bearer_token 或 env_key 指向的环境变量）时，
// CLI 实际可用但登录状态仍显示未登录。这里补充识别这类鉴权方式，避免误报“请先登录”。
function hasCustomProviderAuth() {
  try {
    const config = fs.readFileSync(path.join(os.homedir(), '.codex', 'config.toml'), 'utf8');
    const bearer = config.match(/experimental_bearer_token\s*=\s*"([^"]+)"/);
    if (bearer?.[1]) return true;
    const envKey = config.match(/env_key\s*=\s*"([^"]+)"/);
    return Boolean(envKey?.[1] && process.env[envKey[1]]);
  } catch {
    return false;
  }
}

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

export function createEvents(onProgress, onSession) {
  return jsonLineEvents((event, state) => {
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
  }, onProgress, onSession);
}

const adapter = {
  label: 'Codex', createEvents,
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
      let unchanged = false;
      try { unchanged = gitState(project.path).snapshot === startingGitSnapshot; }
      catch { /* An unreadable or changed repository cannot be retried safely. */ }
      if (!unchanged) return new Error('Codex 对话正被使用，项目文件也发生变化。请先检查改动。');
      const error = new Error('此 Codex 对话正在另一窗口或进程使用；结束后再次确认即可继续');
      error.code = 'CODEX_SESSION_ACTIVE_WRITER';
      error.retryable = true;
      return error;
    }
    if (/model is not supported when using Codex|Model metadata for/.test(output.rawTail)) {
      return new Error('Codex 当前模型不能用于这个登录账号。请在电脑桥接页面的“编程助手模型”中选择该账号可用的模型。');
    }
  },
};

export default {
  apiVersion: 1, kind: 'coding-agent', id: 'codex', label: 'Codex', icon: builtinAgentIcons.fox,
  capabilities: { session: 'native', model: true, cancel: true, progress: true },
  model: { default: 'gpt-6-astra', required: true },
  sessionIdPattern: UUID,
  probe: () => {
    const state = probeCli('codex', ['login', 'status']);
    if (state.available || !hasCustomProviderAuth()) return state;
    return { available: true, mode: 'local_cli', reason: '' };
  },
  run: (context) => runCli(context, adapter),
  createEvents,
  syncJobState(job) {
    if (job.error === 'Codex 当前模型不能用于这个登录账号。请设置 VIBE_CODEX_MODEL 为该账号在 CLI 中可用的模型。') {
      job.error = 'Codex 当前模型不能用于这个登录账号。请在电脑桥接页面的“编程助手模型”中选择该账号可用的模型。';
    }
  },
};
