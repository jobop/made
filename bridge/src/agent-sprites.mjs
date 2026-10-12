/**
 * Agent 形象素材（sprite/）读取与校验。
 *
 * 规范真源是主仓库 docs/SPRITES.md：agent 插件包可选携带 sprite/ 目录
 * （manifest.json + 分层 PNG），桥接器把素材清单暴露给码得，素材本体走
 * /device/sprite 分片拉取端点（单次 ≤192KB，兼容 USB 帧转发 256KB 上限）。
 *
 * 这里只做「读、验、切片」：不改素材、不缓存像素。清单结果按目录 mtime
 * 缓存，避免每次 /device/config 都重算哈希。
 */
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';

export const SPRITE_MAX_TOTAL_BYTES = 512 * 1024;
export const SPRITE_MAX_CHUNK = 192 * 1024;
export const SPRITE_MAX_FILE_BYTES = 256 * 1024;
const MANIFEST_MAX_BYTES = 16 * 1024;
const MAX_LAYERS = 8;
const MAX_FRAMES = 4;
const FILE_NAME = /^[A-Za-z0-9_-]+\.png$/i;
const LAYER_ID = /^[A-Za-z][A-Za-z0-9_]{0,15}$/;
const RESERVED_IDS = new Set(['__proto__', 'constructor', 'prototype']);

class SpriteError extends Error {
  constructor(message) {
    super(message);
    this.preserveMessage = true;
  }
}

const isInt = (value, min, max) => Number.isInteger(value) && value >= min && value <= max;

function readManifestFile(dir) {
  const file = path.join(dir, 'manifest.json');
  let text;
  try {
    text = fs.readFileSync(file, 'utf8');
  } catch {
    return null; // 包没带 sprite/ —— 合法，用固件兜底形象。
  }
  if (Buffer.byteLength(text) > MANIFEST_MAX_BYTES) throw new SpriteError('sprite/manifest.json 超过 16 KB');
  try {
    return JSON.parse(text);
  } catch {
    throw new SpriteError('sprite/manifest.json 不是合法 JSON');
  }
}

function validateManifest(raw) {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) throw new SpriteError('manifest 必须是对象');
  if (Object.keys(raw).some((key) => !['version', 'canvas', 'layers', 'motion'].includes(key))) {
    throw new SpriteError('manifest 包含不支持的字段');
  }
  if (!isInt(raw.version, 1, 2147483647)) throw new SpriteError('manifest.version 必须是正整数');
  const canvas = raw.canvas;
  if (!canvas || typeof canvas !== 'object' || Array.isArray(canvas) ||
      !isInt(canvas.w, 1, 512) || !isInt(canvas.h, 1, 512) ||
      Object.keys(canvas).some((key) => !['w', 'h'].includes(key))) {
    throw new SpriteError('manifest.canvas 必须是 {w, h}（各 1–512）');
  }
  if (!Array.isArray(raw.layers) || raw.layers.length < 1 || raw.layers.length > MAX_LAYERS) {
    throw new SpriteError(`manifest.layers 需要 1–${MAX_LAYERS} 层`);
  }
  const seen = new Set();
  const referenced = new Set();
  for (const layer of raw.layers) {
    if (!layer || typeof layer !== 'object' || Array.isArray(layer)) throw new SpriteError('layers 条目必须是对象');
    const keys = Object.keys(layer);
    if (keys.some((key) => !['id', 'file', 'frames', 'x', 'y', 'z', 'blink', 'talk', 'alert'].includes(key))) {
      throw new SpriteError(`层 ${layer.id || '?'} 包含不支持的字段`);
    }
    if (typeof layer.id !== 'string' || !LAYER_ID.test(layer.id) || RESERVED_IDS.has(layer.id) || seen.has(layer.id)) {
      throw new SpriteError('层 id 必须是 1–16 位字母开头的唯一标识');
    }
    seen.add(layer.id);
    const files = layer.frames !== undefined ? layer.frames : [layer.file];
    if (!Array.isArray(files) || files.length < 1 || files.length > MAX_FRAMES ||
        files.some((file) => typeof file !== 'string' || !FILE_NAME.test(file))) {
      throw new SpriteError(`层 ${layer.id} 需要单帧 file 或 1–${MAX_FRAMES} 帧 frames（PNG 文件名）`);
    }
    for (const file of files) {
      if (referenced.has(file)) throw new SpriteError(`素材文件被多层引用：${file}`);
      referenced.add(file);
    }
    if (!isInt(layer.x, 0, canvas.w) || !isInt(layer.y, 0, canvas.h) || !isInt(layer.z, 0, 100)) {
      throw new SpriteError(`层 ${layer.id} 的 x/y/z 超出画布或范围`);
    }
    if (layer.blink !== undefined) {
      const blink = layer.blink;
      if (!blink || typeof blink !== 'object' || Array.isArray(blink) ||
          !isInt(blink.period_ms, 200, 60000) || !isInt(blink.closed_ms, 20, 2000) ||
          Object.keys(blink).some((key) => !['period_ms', 'closed_ms'].includes(key))) {
        throw new SpriteError(`层 ${layer.id} 的 blink 必须是 {period_ms, closed_ms}`);
      }
    }
    if (layer.talk !== undefined && typeof layer.talk !== 'boolean') {
      throw new SpriteError(`层 ${layer.id} 的 talk 必须是布尔值`);
    }
    if (layer.alert !== undefined && layer.alert !== 'pulse') {
      throw new SpriteError(`层 ${layer.id} 的 alert 只支持 "pulse"`);
    }
  }
  if (raw.motion !== undefined) {
    const motion = raw.motion;
    if (!motion || typeof motion !== 'object' || Array.isArray(motion)) throw new SpriteError('motion 必须是对象');
    const keys = Object.keys(motion);
    if (keys.some((key) => !['breath', 'sway_deg', 'sway_period_ms'].includes(key))) {
      throw new SpriteError('motion 包含不支持的字段');
    }
    if (motion.breath !== undefined) {
      const breath = motion.breath;
      if (!breath || typeof breath !== 'object' || Array.isArray(breath) ||
          !isInt(breath.scale_pct, 100, 120) || !isInt(breath.period_ms, 500, 10000) ||
          Object.keys(breath).some((key) => !['scale_pct', 'period_ms'].includes(key))) {
        throw new SpriteError('motion.breath 必须是 {scale_pct, period_ms}');
      }
    }
    if (motion.sway_deg !== undefined && !isInt(motion.sway_deg, 0, 15)) throw new SpriteError('motion.sway_deg 范围 0–15');
    if (motion.sway_period_ms !== undefined && !isInt(motion.sway_period_ms, 200, 10000)) {
      throw new SpriteError('motion.sway_period_ms 范围 200–10000');
    }
  }
  return { version: raw.version, canvas: canvas, layers: raw.layers, motion: raw.motion ?? null };
}

/** 按目录内容 mtime 缓存读取结果；素材被替换时自动失效。
 * 键要覆盖目录内每个文件：原地覆盖文件不会更新目录自身的 mtime。 */
const cache = new Map();

function spriteStamp(spriteDir) {
  let stamp;
  try {
    stamp = `${fs.statSync(spriteDir).mtimeMs}`;
    for (const name of fs.readdirSync(spriteDir).sort()) {
      stamp += `|${name}:${fs.statSync(path.join(spriteDir, name)).mtimeMs}`;
    }
  } catch {
    return null;
  }
  return stamp;
}

/**
 * 读取并校验一个插件包的 sprite/ 目录。
 * @returns 清单对象（含每个文件的 bytes/sha256）；包没带 sprite/ 时返回 null；
 *          manifest 或素材不合法时抛错（调用方决定是拒绝加载还是降级兜底）。
 */
export function readSprite(packageDir) {
  const spriteDir = path.join(packageDir, 'sprite');
  const stamp = spriteStamp(spriteDir);
  if (!stamp) return null; // 目录不存在：没有素材，合法。
  const hit = cache.get(spriteDir);
  if (hit && hit.stamp === stamp) return hit.value;
  const raw = readManifestFile(spriteDir);
  if (!raw) return null;
  const manifest = validateManifest(raw);
  // 引用完整性：目录里的 PNG 必须全部被 manifest 引用，帮开发者抓住文件名拼写错误。
  for (const name of fs.readdirSync(spriteDir)) {
    if (!name.toLowerCase().endsWith('.png')) continue;
    const referenced = manifest.layers.some((layer) =>
      (layer.frames !== undefined ? layer.frames : [layer.file]).includes(name));
    if (!referenced) throw new SpriteError(`sprite/${name} 未被 manifest 引用`);
  }
  let total = 0;
  const files = [];
  for (const layer of manifest.layers) {
    for (const file of layer.frames !== undefined ? layer.frames : [layer.file]) {
      const target = path.join(spriteDir, file);
      let bytes;
      try {
        bytes = fs.statSync(target).size;
      } catch {
        throw new SpriteError(`manifest 引用的素材不存在：sprite/${file}`);
      }
      if (bytes > SPRITE_MAX_FILE_BYTES) throw new SpriteError(`素材单文件超过 256 KB：${file}`);
      total += bytes;
      if (total > SPRITE_MAX_TOTAL_BYTES) throw new SpriteError(`sprite/ 整包超过 512 KB`);
      const data = fs.readFileSync(target);
      files.push({ file, bytes, sha256: createHash('sha256').update(data).digest('hex') });
    }
  }
  const value = Object.freeze({
    dir: spriteDir, version: manifest.version, canvas: manifest.canvas,
    layers: manifest.layers, motion: manifest.motion,
    files: Object.freeze(files.map((file) => Object.freeze(file))),
  });
  cache.set(spriteDir, { stamp, value });
  return value;
}

/**
 * 从已校验的 sprite 目录切一片文件数据。越界与超长都会收敛到合法范围，
 * 返回空 Buffer 表示 offset 已到文件尾（调用方据此判断下载完成）。
 */
export function spriteChunk(sprite, file, offset, length) {
  const entry = sprite?.files?.find((item) => item.file === file);
  if (!entry) return null;
  const start = Math.max(0, Math.min(Math.trunc(offset) || 0, entry.bytes));
  const wanted = length === undefined || length === null ? SPRITE_MAX_CHUNK : Math.trunc(length) || 0;
  const end = Math.min(entry.bytes, start + Math.max(1, Math.min(wanted, SPRITE_MAX_CHUNK)));
  const data = fs.readFileSync(path.join(sprite.dir, file));
  return data.subarray(start, end);
}
