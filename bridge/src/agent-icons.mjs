/**
 * Small, self-contained avatars shared by the desktop and the round display.
 * Pixels are row-major 4-bit palette indices: high nibble first, no row padding.
 * There are no image URLs, executable SVG or firmware-specific animal names on
 * the wire. Legacy preset names are resolved only inside this bridge module.
 */
export const MAX_ICON_SIZE = 48;
export const MAX_ICON_JSON_BYTES = 2048;
const RGBA = /^[0-9a-f]{8}$/i;
const RGB = /^#[0-9a-f]{6}$/i;
const FIELDS = new Set(['format', 'width', 'height', 'palette', 'data', 'background', 'accent']);
const validated = new WeakSet();

function dimensions(width, height) {
  if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || width > MAX_ICON_SIZE || height > MAX_ICON_SIZE) {
    throw new Error(`图标尺寸必须为 1–${MAX_ICON_SIZE} 像素`);
  }
}

export function validateIndexedIcon(icon) {
  if (icon && validated.has(icon)) return icon;
  if (!icon || typeof icon !== 'object' || Array.isArray(icon) || icon.format !== 'indexed4') throw new Error('图标必须为 indexed4 像素对象');
  if (Object.keys(icon).some(key => !FIELDS.has(key))) throw new Error('图标包含不支持的字段');
  dimensions(icon.width, icon.height);
  if (!Array.isArray(icon.palette) || icon.palette.length < 1 || icon.palette.length > 16 || Array.from(icon.palette).some(color => typeof color !== 'string' || !RGBA.test(color))) {
    throw new Error('图标 palette 需要 1–16 个 RRGGBBAA 颜色');
  }
  if (typeof icon.background !== 'string' || !RGB.test(icon.background) || typeof icon.accent !== 'string' || !RGB.test(icon.accent)) throw new Error('图标背景与强调色必须为 #RRGGBB');
  const pixels = icon.width * icon.height;
  const bytes = Math.ceil(pixels / 2);
  if (typeof icon.data !== 'string' || icon.data.length !== Math.ceil(bytes / 3) * 4 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(icon.data)) throw new Error('图标 data 的 Base64 长度或格式无效');
  const packed = Buffer.from(icon.data, 'base64');
  if (packed.length !== bytes || packed.toString('base64') !== icon.data) throw new Error('图标 data 必须使用标准 Base64 编码');
  for (let i = 0; i < pixels; i++) {
    const index = i % 2 ? packed[i >> 1] & 15 : packed[i >> 1] >> 4;
    if (index >= icon.palette.length) throw new Error('图标像素引用了不存在的 palette 颜色');
  }
  if (pixels % 2 && (packed.at(-1) & 15)) throw new Error('图标最后一个未使用的低半字节必须为零');
  const normalized = Object.freeze({
    format: 'indexed4', width: icon.width, height: icon.height,
    palette: Object.freeze(icon.palette.map(color => color.toLowerCase())), data: icon.data,
    background: icon.background.toLowerCase(), accent: icon.accent.toLowerCase(),
  });
  if (Buffer.byteLength(JSON.stringify(normalized)) > MAX_ICON_JSON_BYTES) throw new Error('图标数据超过 2 KB');
  validated.add(normalized);
  return normalized;
}

/** Convert bounded raw RGBA bytes to an avatar; deterministic median-cut when
 * the source contains more than 16 colors. Use a developer image tool to resize
 * a PNG first; the running bridge does not decode arbitrary image files. */
export function rgbaToIndexed4({ width, height, pixels, background = '#25373f', accent = '#b7e3df' }) {
  dimensions(width, height);
  if (!(pixels instanceof Uint8Array) || pixels.length !== width * height * 4) throw new Error('RGBA 数据长度必须为 width × height × 4');
  const colors = new Map();
  const source = [];
  for (let i = 0; i < pixels.length; i += 4) {
    const rgba = pixels[i + 3] === 0 ? [0, 0, 0, 0] : [...pixels.subarray(i, i + 4)];
    const key = rgba.join(',');
    if (!colors.has(key)) colors.set(key, { rgba, count: 0 });
    colors.get(key).count++;
    source.push(key);
  }
  let boxes = [[...colors.values()]];
  const span = (box, channel) => Math.max(...box.map(c => c.rgba[channel])) - Math.min(...box.map(c => c.rgba[channel]));
  while (boxes.length < 16) {
    let candidate = -1, channel = 0, score = -1;
    boxes.forEach((box, i) => {
      if (box.length < 2) return;
      for (let c = 0; c < 4; c++) {
        const next = span(box, c) * Math.sqrt(box.reduce((sum, color) => sum + color.count, 0));
        if (next > score) { score = next; candidate = i; channel = c; }
      }
    });
    if (candidate < 0) break;
    const box = boxes[candidate].sort((a, b) => a.rgba[channel] - b.rgba[channel]);
    const half = box.reduce((sum, color) => sum + color.count, 0) / 2;
    let total = 0, split = 1;
    for (let i = 0; i < box.length - 1; i++) { total += box[i].count; split = i + 1; if (total >= half) break; }
    boxes.splice(candidate, 1, box.slice(0, split), box.slice(split));
  }
  const palette = [], indexes = new Map();
  for (const box of boxes) {
    const weight = box.reduce((sum, color) => sum + color.count, 0);
    const rgba = [0, 1, 2, 3].map(channel => Math.round(box.reduce((sum, color) => sum + color.rgba[channel] * color.count, 0) / weight));
    for (const color of box) indexes.set(color.rgba.join(','), palette.length);
    palette.push(rgba.map(value => value.toString(16).padStart(2, '0')).join(''));
  }
  const packed = Buffer.alloc(Math.ceil(source.length / 2));
  source.forEach((key, i) => { packed[i >> 1] |= indexes.get(key) << (i % 2 ? 0 : 4); });
  return validateIndexedIcon({ format: 'indexed4', width, height, palette, data: packed.toString('base64'), background, accent });
}

// Original 42 × 42 artwork is maintained here, never inside device firmware.
const artwork = {
  fox: { background: '#503728', accent: '#ffb264', shapes: [
    [6,3,12,18,'#d87935',3], [24,3,12,18,'#d87935',3], [9,7,6,9,'#ffcca7',3], [27,7,6,9,'#ffcca7',3], [5,12,32,26,'#ffac5b',16], [9,26,24,11,'#fff1df',8], [13,21,3,4,'#26364b',3], [26,21,3,4,'#26364b',3], [19,30,5,4,'#664343',3],
  ] },
  rabbit: { background: '#413756', accent: '#c5abff', shapes: [
    [9,1,9,24,'#f3ebff',6], [24,1,9,24,'#f3ebff',6], [12,5,3,14,'#e6a9d5',3], [27,5,3,14,'#e6a9d5',3], [6,16,30,23,'#f3ebff',14], [13,25,3,4,'#35354d',3], [26,25,3,4,'#35354d',3], [19,31,5,4,'#d586ba',3],
  ] },
  owl: { background: '#254b54', accent: '#60ddd2', shapes: [
    [6,5,11,18,'#238d98',4], [25,5,11,18,'#238d98',4], [5,11,32,28,'#4cc9c7',14], [8,19,12,13,'#fff9e9',7], [22,19,12,13,'#fff9e9',7], [13,23,4,5,'#214e64',3], [26,23,4,5,'#214e64',3], [19,31,5,5,'#ffc269',2],
  ] },
  panda: { background: '#36445d', accent: '#d7e4f5', shapes: [
    [4,7,12,12,'#293a54',6], [26,7,12,12,'#293a54',6], [6,12,30,26,'#f5f8ff',15], [10,20,10,12,'#293a54',6], [22,20,10,12,'#293a54',6], [14,23,3,4,'#f5f8ff',3], [26,23,3,4,'#f5f8ff',3], [19,31,5,4,'#293a54',3],
  ] },
  generic: { background: '#25373f', accent: '#b7e3df', shapes: [
    [5,8,32,24,'#b7e3df',9], [10,25,9,11,'#b7e3df',2], [11,17,4,5,'#25373f',2], [19,17,4,5,'#25373f',2], [27,17,4,5,'#25373f',2],
  ] },
};

function renderArtwork({ background, accent, shapes }) {
  const size = 48, pixels = new Uint8Array(size * size * 4);
  for (const [x, y, width, height, fill, round] of shapes) {
    const rx = Math.min(round, width / 2), ry = Math.min(round, height / 2);
    const color = [1, 3, 5].map(offset => parseInt(fill.slice(offset, offset + 2), 16));
    for (let py = 0; py < size; py++) for (let px = 0; px < size; px++) {
      const dx = (px + .5) * 42 / size - x, dy = (py + .5) * 42 / size - y;
      if (dx < 0 || dy < 0 || dx >= width || dy >= height) continue;
      const cx = Math.max(rx - dx, dx - (width - rx), 0), cy = Math.max(ry - dy, dy - (height - ry), 0);
      if (cx && cy && (cx / rx) ** 2 + (cy / ry) ** 2 > 1) continue;
      pixels.set([...color, 255], (py * size + px) * 4);
    }
  }
  return rgbaToIndexed4({ width: size, height: size, pixels, background, accent });
}
export const builtinAgentIcons = Object.freeze(Object.fromEntries(Object.entries(artwork).map(([id, image]) => [id, renderArtwork(image)])));

/** Legacy string aliases are accepted for existing desktop plugins only. */
export function normalizeAgentIcon(icon) {
  if (icon === undefined || icon === null) return builtinAgentIcons.generic;
  if (typeof icon === 'string') {
    if (!Object.hasOwn(builtinAgentIcons, icon)) throw new Error('未知的旧图标名称；请提供 indexed4 像素对象');
    return builtinAgentIcons[icon];
  }
  return validateIndexedIcon(icon);
}
