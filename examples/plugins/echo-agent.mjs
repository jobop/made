// A local, non-coding demonstration of persistent conversation context.
import fs from 'node:fs/promises';
import path from 'node:path';
import { randomUUID } from 'node:crypto';
import icon from './echo-icon.json' with { type: 'json' };

/** @type {import('../../src/plugins/contracts.d.ts').CodingAgentPlugin} */
export default {
  apiVersion: 1, kind: 'coding-agent', id: 'echo-example', label: '上下文示例', icon,
  capabilities: { session: 'native', model: false, cancel: true, progress: true },
  sessionIdPattern: /^[0-9a-f-]{36}$/,
  probe: () => ({ available: true, reason: '本地演示，不执行编程任务' }),
  async run({ job, config, signal, onProgress, onSession }) {
    signal.throwIfAborted();
    const id = job.sessionId || randomUUID();
    onSession(id);
    onProgress('正在读取同一任务的上下文');
    const directory = path.join(config.stateDir, 'plugins', 'echo-example');
    await fs.mkdir(directory, { recursive: true, mode: 0o700 });
    const file = path.join(directory, `${id}.json`);
    let previous = '';
    try { previous = JSON.parse(await fs.readFile(file, 'utf8')).lastMessage || ''; }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    signal.throwIfAborted();
    await fs.writeFile(file, JSON.stringify({ lastMessage: job.instruction }), { mode: 0o600 });
    return { status: 'completed', result: `${previous ? `上一句：${previous}\n` : ''}这一句：${job.instruction}\n（本地插件示例，不执行代码）` };
  },
};
