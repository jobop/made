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
const MAX_FRAMES = 8;
// 情绪表允许的状态名（与固件 Mood 枚举一一对应）。
const MOOD_NAMES = new Set(['idle', 'talking', 'running', 'waiting', 'success', 'sad', 'offline']);
// ≤20 字符：固件 SPIFFS 缓存对象名 = '/' + "s<hash6>_" + 文件名 + "~"，
// 而 SPIFFS 只给 31 字节，20 是留了 1 字节余量的上限。
const FILE_NAME = /^[A-Za-z0-9_-]{1,16}\.png$/i;
const GIF_NAME = /^[A-Za-z0-9_-]{1,16}\.gif$/i;
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
  if (Object.keys(raw).some((key) => !['version', 'canvas', 'layers', 'motion', 'emotions', 'actions'].includes(key))) {
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
  if (raw.emotions !== undefined) {
    const emotions = raw.emotions;
    if (!emotions || typeof emotions !== 'object' || Array.isArray(emotions)) {
      throw new SpriteError('emotions 必须是对象');
    }
    for (const [mood, entry] of Object.entries(emotions)) {
      if (!MOOD_NAMES.has(mood)) throw new SpriteError(`emotions 含未知状态：${mood}`);
      if (!entry || typeof entry !== 'object' || Array.isArray(entry) ||
          Object.keys(entry).length < 1) {
        throw new SpriteError(`emotions.${mood} 必须是非空对象（层id → 帧序号 | "cycle"）`);
      }
      for (const [layerId, sel] of Object.entries(entry)) {
        const layer = raw.layers.find((item) => item.id === layerId);
        if (!layer) throw new SpriteError(`emotions.${mood} 引用未知层：${layerId}`);
        const frameCount = (layer.frames !== undefined ? layer.frames : [layer.file]).length;
        if (sel === 'cycle') continue;
        if (!isInt(sel, 0, frameCount - 1)) {
          throw new SpriteError(`emotions.${mood}.${layerId} 需要帧序号 0–${frameCount - 1} 或 "cycle"`);
        }
      }
    }
  }
  // 行动系统：actions 表（可选）。两种动作形态二选一：
  //   帧表动作 = { frame_ms, loops, tracks }（对既有层帧序的关键帧覆盖）；
  //   GIF 动作  = { gif, ms }（整帧动画文件，播放 ms 毫秒）。
  // 上限与固件一致：≤16 动作 / 每动作 ≤8 轨道 / 每轨道 ≤32 步。
  const gifReferenced = new Set();
  if (raw.actions !== undefined) {
    const actions = raw.actions;
    if (!actions || typeof actions !== 'object' || Array.isArray(actions)) {
      throw new SpriteError('actions 必须是对象');
    }
    const names = Object.keys(actions);
    if (names.length > 16) throw new SpriteError('actions 最多 16 个动作');
    for (const [name, action] of Object.entries(actions)) {
      if (!/^[a-z][a-z0-9_]{0,15}$/.test(name)) throw new SpriteError(`actions.${name} 名字须为小写字母开头的 [a-z0-9_]{0,15}`);
      if (!action || typeof action !== 'object' || Array.isArray(action)) {
        throw new SpriteError(`actions.${name} 必须是对象`);
      }
      if (action.gif !== undefined) {
        // GIF 动作：整帧动画，ms 是播放时长（设备端流式解码，文件不分帧）
        if (Object.keys(action).some((key) => !['gif', 'ms'].includes(key))) {
          throw new SpriteError(`actions.${name} 包含不支持的字段（GIF 动作只支持 gif/ms）`);
        }
        if (typeof action.gif !== 'string' || !GIF_NAME.test(action.gif)) {
          throw new SpriteError(`actions.${name}.gif 需要 1–16 位 [A-Za-z0-9_-] 的 .gif 文件名`);
        }
        if (action.ms !== undefined && !isInt(action.ms, 100, 10000)) {
          throw new SpriteError(`actions.${name}.ms 范围 100–10000`);
        }
        gifReferenced.add(action.gif);
        continue;
      }
      if (Object.keys(action).some((key) => !['frame_ms', 'loops', 'tracks'].includes(key))) {
        throw new SpriteError(`actions.${name} 包含不支持的字段`);
      }
      if (action.frame_ms !== undefined && !isInt(action.frame_ms, 30, 2000)) {
        throw new SpriteError(`actions.${name}.frame_ms 范围 30–2000`);
      }
      if (action.loops !== undefined && !isInt(action.loops, 1, 10)) {
        throw new SpriteError(`actions.${name}.loops 范围 1–10`);
      }
      const tracks = action.tracks;
      if (!tracks || typeof tracks !== 'object' || Array.isArray(tracks) ||
          Object.keys(tracks).length < 1 || Object.keys(tracks).length > 8) {
        throw new SpriteError(`actions.${name}.tracks 需要 1–8 条轨道（层id → 帧序号数组）`);
      }
      for (const [layerId, steps] of Object.entries(tracks)) {
        const layer = raw.layers.find((item) => item.id === layerId);
        if (!layer) throw new SpriteError(`actions.${name} 引用未知层：${layerId}`);
        const frameCount = (layer.frames !== undefined ? layer.frames : [layer.file]).length;
        if (!Array.isArray(steps) || steps.length < 1 || steps.length > 32 ||
            steps.some((step) => !isInt(step, 0, frameCount - 1))) {
          throw new SpriteError(`actions.${name}.tracks.${layerId} 需要 1–32 个帧序号（0–${frameCount - 1}）`);
        }
      }
    }
  }
  return {
    version: raw.version, canvas: canvas, layers: raw.layers, motion: raw.motion ?? null,
    emotions: raw.emotions ?? null, actions: raw.actions ?? null,
  };
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
  // 引用完整性：目录里的 PNG 必须全部被 manifest 层引用、GIF 必须被某个
  // GIF 动作引用，帮开发者抓住文件名拼写错误。
  const gifReferenced = new Set();
  if (manifest.actions) {
    for (const action of Object.values(manifest.actions)) {
      if (action && typeof action.gif === 'string') gifReferenced.add(action.gif);
    }
  }
  for (const name of fs.readdirSync(spriteDir)) {
    const lower = name.toLowerCase();
    if (lower.endsWith('.png')) {
      const referenced = manifest.layers.some((layer) =>
        (layer.frames !== undefined ? layer.frames : [layer.file]).includes(name));
      if (!referenced) throw new SpriteError(`sprite/${name} 未被 manifest 引用`);
    } else if (lower.endsWith('.gif')) {
      if (!gifReferenced.has(name)) throw new SpriteError(`sprite/${name} 未被任何 GIF 动作引用`);
    }
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
  // GIF 动作引用的动画文件（多个动作共用同一文件只下发一份）
  if (manifest.actions) {
    const listed = new Set(files.map((entry) => entry.file));
    for (const action of Object.values(manifest.actions)) {
      if (!action || typeof action.gif !== 'string' || listed.has(action.gif)) continue;
      const target = path.join(spriteDir, action.gif);
      let bytes;
      try {
        bytes = fs.statSync(target).size;
      } catch {
        throw new SpriteError(`GIF 动作引用的素材不存在：sprite/${action.gif}`);
      }
      if (bytes > SPRITE_MAX_FILE_BYTES) throw new SpriteError(`素材单文件超过 256 KB：${action.gif}`);
      total += bytes;
      if (total > SPRITE_MAX_TOTAL_BYTES) throw new SpriteError(`sprite/ 整包超过 512 KB`);
      const data = fs.readFileSync(target);
      files.push({ file: action.gif, bytes, sha256: createHash('sha256').update(data).digest('hex') });
    }
  }
  const value = Object.freeze({
    dir: spriteDir, version: manifest.version, canvas: manifest.canvas,
    layers: manifest.layers, motion: manifest.motion, emotions: manifest.emotions,
    actions: manifest.actions,
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
