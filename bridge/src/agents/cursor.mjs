import { builtinAgentIcons } from '../agent-icons.mjs';
import { probeCli, runCli } from './shared.mjs';
import { jsonLineEvents, assistantEvent, streamActivity } from './events.mjs';

export function createEvents(onProgress, onSession) {
  return jsonLineEvents((event, state) => {
    assistantEvent(event, state, { append: true });
    return { sessionId: event.session_id, activity: streamActivity(event) };
  }, onProgress, onSession);
}

const adapter = {
  label: 'Cursor', createEvents,
  command({ cwd, instruction, model, sessionId }) {
    const args = ['--print', '--trust', '--output-format', 'stream-json', '--stream-partial-output', '--sandbox', 'enabled', '--auto-review', '--workspace', cwd];
    if (sessionId) args.push('--resume', sessionId);
    if (model) args.push('--model', model);
    args.push(instruction);
    return { command: 'cursor-agent', args };
  },
};

export default {
  apiVersion: 1, kind: 'coding-agent', id: 'cursor', label: 'Cursor', icon: builtinAgentIcons.rabbit,
  capabilities: { session: 'native', model: true, cancel: true, progress: true },
  model: { default: '', required: false },
  sessionIdPattern: /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i,
  probe: () => probeCli('cursor-agent', ['status']),
  run: (context) => runCli(context, adapter),
  createEvents,
};
