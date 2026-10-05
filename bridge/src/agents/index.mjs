import codex from './codex.mjs';
import cursor from './cursor.mjs';
import qoder from './qoder.mjs';
import workbuddy from './workbuddy.mjs';

export const builtinAgents = Object.freeze([codex, cursor, qoder, workbuddy]);
export function builtinAgent(id) { return builtinAgents.find((plugin) => plugin.id === id); }
