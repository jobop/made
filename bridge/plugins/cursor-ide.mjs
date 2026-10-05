// Cursor IDE 接入插件（只读镜像 + 打开工作区）
//
// 内置的 cursor 助手走 cursor-agent CLI；本插件支持另一种方式：
// 任务确认后自动在 Cursor IDE 中打开项目目录，并只读轮询 IDE 的
// 会话数据库（globalStorage/state.vscdb），把 IDE 里的新消息回显
// 到电脑工作台和码得。不改任何 IDE 数据，schema 变化时安全降级。
//
// 限制：IDE 会话无法从外部注入指令，码得提交的文字不会替你输入到
// IDE；本插件是"打开 + 镜像"，实际编程在 IDE 中进行。

import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { spawn, spawnSync } from 'node:child_process';
import { builtinAgentIcons } from '../src/agent-icons.mjs';

const BUBBLE_TEXT_LIMIT = 300;
const APPEND_LIMIT = 4000;
const MAX_MATCHED_SESSIONS = 1;

function cursorDbPath() {
  const home = process.env.HOME || '';
  if (process.platform === 'darwin') {
    return path.join(home, 'Library/Application Support/Cursor/User/globalStorage/state.vscdb');
  }
  if (process.platform === 'win32') {
    return path.join(process.env.APPDATA || '', 'Cursor/User/globalStorage/state.vscdb');
  }
  return path.join(home, '.config/Cursor/User/globalStorage/state.vscdb');
}

function userStorageDir() {
  return path.dirname(path.dirname(cursorDbPath()));
}

function query(db, sql) {
  const result = spawnSync('sqlite3', ['-json', db, sql], { encoding: 'utf8', timeout: 5000 });
  if (result.status !== 0 || !result.stdout.trim()) return [];
  try { return JSON.parse(result.stdout); } catch { return []; }
}

// workspaceId → 文件夹路径（workspaceStorage/<id>/workspace.json）
function workspaceFolders() {
  const map = new Map();
  const dir = path.join(userStorageDir(), 'workspaceStorage');
  let entries = [];
  try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return map; }
  for (const entry of entries) {
    if (!entry.isDirectory()) continue;
    try {
      const file = path.join(dir, entry.name, 'workspace.json');
      const parsed = JSON.parse(fs.readFileSync(file, 'utf8'));
      const folder = typeof parsed.folder === 'string' ? parsed.folder : '';
      if (!folder.startsWith('file://')) continue;
      const local = decodeURIComponent(folder.replace(/^file:\/\//, ''));
      map.set(entry.name, local);
    } catch { /* 跳过无法解析的工作区 */ }
  }
  return map;
}

function sqlString(value) {
  return `'${String(value).replace(/'/g, "''")}'`;
}

// 找到目标工作区下、基线之后活跃的会话（排除草稿与子代理）
function findActiveSessions(db, workspacePath, baseline) {
  const folderIds = [];
  for (const [id, folder] of workspaceFolders()) {
    if (folder === workspacePath) folderIds.push(id);
  }
  if (!folderIds.length) return [];
  const list = folderIds.map(sqlString).join(', ');
  const rows = query(db,
    `SELECT composerId, value FROM composerHeaders
     WHERE workspaceId IN (${list}) AND isArchived = 0 AND isSubagent = 0
       AND lastUpdatedAt > ${Number(baseline)}
     ORDER BY lastUpdatedAt DESC LIMIT 10;`);
  const sessions = [];
  for (const row of rows) {
    try {
      const head = JSON.parse(row.value || '{}');
      if (head.isDraft) continue;
      sessions.push({ composerId: row.composerId, updatedAt: head.lastUpdatedAt ?? 0 });
    } catch { /* 跳过 */ }
  }
  return sessions;
}

// 读取会话中 rowid 之后的新消息（json_extract 只取 type/text，避免拉整块大 JSON）
function readNewBubbles(db, composerId, afterRowid) {
  const rows = query(db,
    `SELECT rowid, json_extract(value, '$.type') AS type, json_extract(value, '$.text') AS text
     FROM cursorDiskKV
     WHERE key LIKE 'bubbleId:${sqlString(composerId).slice(1, -1)}:%' AND rowid > ${Number(afterRowid)}
     ORDER BY rowid LIMIT 40;`);
  const messages = [];
  let maxRowid = afterRowid;
  for (const row of rows) {
    if (row.rowid > maxRowid) maxRowid = row.rowid;
    const text = typeof row.text === 'string' ? row.text.trim() : '';
    if (!text) continue;
    messages.push({
      role: row.type === 1 ? 'user' : 'assistant',
      text: text.slice(0, BUBBLE_TEXT_LIMIT),
    });
    if (messages.length >= 10) break;
  }
  return { messages, maxRowid };
}

async function run({ job, project, config }) {
  const locale = config.locale === 'en' ? 'en' : 'zh-CN';
  const target = project.path;
  if (process.platform === 'darwin') {
    spawn('open', ['-a', 'Cursor', target], { detached: true, stdio: 'ignore' }).unref();
  } else {
    const child = spawn('cursor', [target], { detached: true, stdio: 'ignore' });
    child.on('error', () => { /* 未安装 cursor 命令时忽略 */ });
    child.unref();
  }
  const result = locale === 'en'
    ? `Opened ${target} in Cursor IDE. Continue the session there; new messages will be mirrored here.`
    : `已在 Cursor IDE 打开 ${target}。请在 IDE 中继续这个会话，新的对话会镜像回这里。`;
  return {
    status: 'handed_off',
    result,
    handoff: { cursor: '', readState: 'retrying', replyCount: 0,
      workspacePath: target, baseline: Date.now(), lastRowid: 0 },
  };
}

async function readUpdates({ job, config }) {
  const locale = config.locale === 'en' ? 'en' : 'zh-CN';
  const handoff = job.handoff || {};
  if (handoff.readState === 'ambiguous' || handoff.readState === 'unavailable') return {};
  const db = cursorDbPath();
  if (!fs.existsSync(db)) {
    return { handoff: { ...handoff, readState: 'unavailable' } };
  }
  const active = findActiveSessions(db, handoff.workspacePath || '', handoff.baseline || 0);
  if (!active.length) return {};
  if (active.length > MAX_MATCHED_SESSIONS) {
    // 无法确定归属时停止回读，避免把其他会话的内容当作本任务结果。
    return { handoff: { ...handoff, readState: 'ambiguous' } };
  }
  const session = active[0];
  const { messages, maxRowid } = readNewBubbles(db, session.composerId, handoff.lastRowid || 0);
  if (!messages.length) {
    return { handoff: { ...handoff, cursor: session.composerId, readState: 'retrying' } };
  }
  const lines = messages.map((m) => locale === 'en'
    ? `${m.role === 'user' ? 'IDE you' : 'Cursor'}: ${m.text}`
    : `${m.role === 'user' ? 'IDE 你' : 'Cursor'}: ${m.text}`);
  const appendResult = lines.join('\n\n').slice(-APPEND_LIMIT);
  return {
    handoff: { ...handoff, cursor: session.composerId, readState: 'retrying',
      replyCount: (handoff.replyCount || 0) + messages.length, lastRowid: maxRowid },
    appendResult,
  };
}

export default {
  apiVersion: 1,
  kind: 'coding-agent',
  id: 'cursor-ide',
  label: 'Cursor IDE',
  icon: builtinAgentIcons.owl,
  capabilities: { session: 'external_unscoped', model: false, cancel: false, progress: false },
  probe: () => {
    const db = cursorDbPath();
    if (!fs.existsSync(db)) {
      return { available: false, reason: '未找到 Cursor IDE 的本地数据，请确认已安装并运行过 Cursor' };
    }
    return { available: true, mode: 'ide_mirror' };
  },
  run,
  readUpdates,
};
