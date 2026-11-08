import fs from 'node:fs';
import path from 'node:path';
import { randomUUID } from 'node:crypto';

/**
 * APP 侧传图：图片落在「会话工作目录/uploads/」，也就是助手 CLI 的 cwd。
 * 这样图片同时有三重用途：任务里能回显、会话删除时自动清理、助手能用相对
 * 路径读取。板端固件不使用这套接口（它只消费 GET /device/tasks 的字段白名单）。
 */
export const MAX_IMAGE_BYTES = 6 * 1024 * 1024;
export const MAX_TASK_IMAGES = 4;
export const IMAGE_ID_RE = /^[a-f0-9]{12}$/;
const MAX_FILES_PER_SESSION = 120;

const FORMATS = [
  {
    ext: 'jpg', mime: 'image/jpeg',
    match: (b) => b.length > 3 && b[0] === 0xff && b[1] === 0xd8 && b[2] === 0xff,
  },
  {
    ext: 'png', mime: 'image/png',
    match: (b) => b.length > 8 && b[0] === 0x89 && b[1] === 0x50 && b[2] === 0x4e &&
      b[3] === 0x47 && b[4] === 0x0d && b[5] === 0x0a && b[6] === 0x1a && b[7] === 0x0a,
  },
  {
    ext: 'webp', mime: 'image/webp',
    match: (b) => b.length > 12 && b.toString('ascii', 0, 4) === 'RIFF' &&
      b.toString('ascii', 8, 12) === 'WEBP',
  },
];

function safeName(value) {
  return String(value || 'image').replace(/[\\/\x00-\x1f\x7f]/g, '_')
    .replace(/^\.+/, '_').slice(0, 80) || 'image';
}

function uploadsDir(sessionDir, create = false) {
  // 会话根与 uploads 都必须是真目录；助手能在回合间改写它们。
  if (fs.lstatSync(sessionDir).isSymbolicLink()) throw new Error('图片目录不能是符号链接');
  const session = fs.realpathSync(sessionDir);
  const dir = path.join(session, 'uploads');
  if (create && !fs.existsSync(dir)) fs.mkdirSync(dir, { mode: 0o700 });
  const stat = fs.lstatSync(dir);
  if (!stat.isDirectory() || stat.isSymbolicLink() || path.dirname(fs.realpathSync(dir)) !== session) {
    throw new Error('图片目录不能是符号链接或指向会话外部');
  }
  return dir;
}

function regularFile(file, maxBytes) {
  const stat = fs.lstatSync(file);
  if (!stat.isFile() || stat.isSymbolicLink() || stat.size > maxBytes) throw new Error('图片文件无效');
  return stat;
}

function readRegular(file, maxBytes) {
  regularFile(file, maxBytes);
  const fd = fs.openSync(file, fs.constants.O_RDONLY | (fs.constants.O_NOFOLLOW || 0));
  try {
    const stat = fs.fstatSync(fd);
    if (!stat.isFile() || stat.size > maxBytes) throw new Error('图片文件无效');
    // 有界读取，即使文件在检查后增长也不会无限分配内存。
    const bytes = Buffer.alloc(stat.size);
    let offset = 0;
    while (offset < bytes.length) {
      const n = fs.readSync(fd, bytes, offset, bytes.length - offset, offset);
      if (!n) break;
      offset += n;
    }
    return bytes.subarray(0, offset);
  } finally { fs.closeSync(fd); }
}

function storedImage(sessionDir, id) {
  const dir = uploadsDir(sessionDir);
  const meta = JSON.parse(readRegular(path.join(dir, `${id}.json`), 16 * 1024).toString('utf8'));
  const format = formatFor(meta?.mime);
  if (!format) throw new Error('图片数据损坏');
  const file = path.join(dir, `${id}.${format.ext}`);
  const stat = regularFile(file, MAX_IMAGE_BYTES);
  return { meta, format, file, stat };
}

function formatFor(mime) {
  return FORMATS.find((f) => f.mime === mime);
}

/** 相对地址：APP 把它拼到自己的 baseUrl 上，云端 /bridge/ 前缀也照常生效。 */
export function imageUrl(sessionId, id) {
  return `/device/images/${id}?sessionId=${encodeURIComponent(sessionId)}`;
}

export function saveImage(sessionDir, sessionId, bytes, name) {
  if (!Buffer.isBuffer(bytes) || bytes.length === 0) throw new Error('图片内容为空');
  if (bytes.length > MAX_IMAGE_BYTES) throw new Error('图片超过 6MB 上限');
  // 只认文件头，不信客户端声明的 Content-Type，避免把脚本当图片落盘。
  const format = FORMATS.find((f) => f.match(bytes));
  if (!format) throw new Error('只支持 JPG / PNG / WebP 图片');
  const dir = uploadsDir(sessionDir, true);
  const stored = fs.readdirSync(dir).filter((f) => f.endsWith('.json') &&
    IMAGE_ID_RE.test(f.slice(0, -5)));
  if (stored.length >= MAX_FILES_PER_SESSION) throw new Error('本会话图片过多，请新建会话');
  const id = randomUUID().replace(/-/g, '').slice(0, 12);
  const meta = {
    id,
    name: safeName(name),
    mime: format.mime,
    size: bytes.length,
    createdAt: new Date().toISOString(),
  };
  fs.writeFileSync(path.join(dir, `${id}.${format.ext}`), bytes, { mode: 0o600, flag: 'wx' });
  fs.writeFileSync(path.join(dir, `${id}.json`), JSON.stringify(meta), { mode: 0o600, flag: 'wx' });
  return { ...meta, url: imageUrl(sessionId, id), file: `uploads/${id}.${format.ext}` };
}

/** 读回图片供 APP 回显。id 先过白名单正则，杜绝路径穿越。 */
export function readImage(sessionDir, id) {
  if (!IMAGE_ID_RE.test(String(id || ''))) return null;
  try {
    const { meta, format, file } = storedImage(sessionDir, id);
    return { bytes: readRegular(file, MAX_IMAGE_BYTES), mime: format.mime, meta };
  } catch { return null; }
}

/** 提交任务时把客户端给的 id 换成已落盘的真实文件信息；跨会话 id 一律不认。 */
export function resolveTaskImages(sessionDir, sessionId, ids) {
  if (ids === undefined || ids === null) return [];
  if (!Array.isArray(ids)) throw new Error('图片参数无效');
  if (ids.length > MAX_TASK_IMAGES) throw new Error(`一条任务最多 ${MAX_TASK_IMAGES} 张图片`);
  const out = [];
  for (const raw of ids) {
    const id = String(raw || '');
    if (!IMAGE_ID_RE.test(id)) throw new Error('图片编号无效');
    let meta, format;
    try { ({ meta, format } = storedImage(sessionDir, id)); }
    catch { throw new Error('图片不存在或已清理'); }
    out.push({
      id,
      name: String(meta.name || 'image').slice(0, 80),
      url: imageUrl(sessionId, id),
      file: `uploads/${id}.${format.ext}`,
    });
  }
  return out;
}

/** 任务持久化字段的防御性清洗（桌面 /api/jobs 也会经过 submit）。 */
export function sanitizeInputImages(value) {
  if (!Array.isArray(value)) return [];
  return value.slice(0, MAX_TASK_IMAGES).map((a) => ({
    id: String(a?.id || ''),
    name: String(a?.name || '').slice(0, 80),
    url: String(a?.url || '').slice(0, 512),
    file: String(a?.file || '').slice(0, 128),
  })).filter((a) => IMAGE_ID_RE.test(a.id));
}

/**
 * 给助手插件用的执行副本：file 解析成会话工作目录下的绝对路径（插件的 cwd 是
 * 项目目录，读不到相对路径），仍限制在 uploads/ 内防目录穿越。没有图或目录
 * 不可用就返回 undefined，任务对象保持原样。
 */
export function absolutizeInputImages(sessionDir, images) {
  const list = sanitizeInputImages(images).filter((a) => a.file);
  if (!list.length || !sessionDir) return undefined;
  const out = [];
  for (const item of list) {
    try {
      const { format, stat } = storedImage(sessionDir, item.id);
      // 只接受该编号的规范路径，不接受嵌套目录、其他编号或替换后的文件。
      if (item.file !== `uploads/${item.id}.${format.ext}`) continue;
      out.push({ ...item, file: path.resolve(sessionDir, item.file), mime: format.mime, bytes: stat.size });
    } catch { /* 已删除、目录/文件被换成链接：不交给助手 */ }
  }
  return out.length ? out : undefined;
}

/**
 * 图片提示只加在「执行副本」上，任务里保存的用户原文不动（与 onTurnInput
 * 管道的语义一致）。图片本体由助手插件以多模态消息原生带给模型（pi SDK 的
 * session.prompt(text, { images })），这里的提示只负责告知「图已附上」，不
 * 再让模型去工作目录读文件。
 */
export function imagePrompt(images) {
  const list = sanitizeInputImages(images).filter((a) => a.file);
  if (!list.length) return '';
  const names = list.map((a) => a.name || a.file).join('、');
  return `\n\n[用户附带的图片] 用户同时发送了 ${list.length} 张图片（${names}），已随本条消息附上。请先看图，再结合上面的指令回答；回答必须基于图片实际内容，不要凭空补充。`;
}

export function withImagePrompt(instruction, images) {
  const hint = imagePrompt(images);
  return hint ? `${String(instruction || '')}${hint}` : String(instruction || '');
}
