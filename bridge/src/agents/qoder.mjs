import { builtinAgentIcons } from '../agent-icons.mjs';
import { probeCli, runCli } from './shared.mjs';
import { jsonLineEvents, assistantEvent, streamActivity } from './events.mjs';

export function createEvents(onProgress, onSession) {
  return jsonLineEvents((event, state) => {
    assistantEvent(event, state);
    return {
      sessionId: event.type === 'system' && event.subtype === 'init' ? event.session_id || event.data?.session_id : undefined,
      activity: streamActivity(event),
    };
  }, onProgress, onSession);
}

const adapter = {
  label: 'Qoder', createEvents,
  command({ cwd, instruction, model, sessionId }) {
    const args = ['--print', '--output-format', 'stream-json', '--permission-mode', 'accept_edits', '--cwd', cwd];
    if (sessionId) args.push('--session-id', sessionId);
    if (model) args.push('--model', model);
    args.push(instruction);
    return { command: 'qoder', args };
  },
};

export default {
  apiVersion: 1, kind: 'coding-agent', id: 'qoder', label: 'Qoder', icon: builtinAgentIcons.owl,
  capabilities: { session: 'native', model: true, cancel: true, progress: true },
  model: { default: '', required: false },
  sessionIdPattern: /^[A-Za-z0-9_-]{6,128}$/,
  probe: () => probeCli('qoder'),
  run: (context) => runCli(context, adapter),
  createEvents,
};
