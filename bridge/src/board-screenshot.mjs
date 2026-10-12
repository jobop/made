// 板端截屏：固件把当前 LVGL 画面以 RGB565 裸帧 POST 上来，这里转 PNG 落盘。
// 触发端走桌面本地 API（POST /api/screenshot），向设备队列塞 debug.screenshot 命令。
// 仅调试用途：命令在固件能力清单里声明（esp_brookesia_app_vibe.cpp announceBoardCatalog）。
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';

const MAX_WIDTH = 1024;
const MAX_HEIGHT = 1024;
const MAX_UPLOAD = 2 * 1024 * 1024;
const KEEP_FILES = 20;

const crcTable = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n += 1) {
    let c = n;
    for (let k = 0; k < 8; k += 1) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();

function crc32(buffer) {
  let crc = 0xffffffff;
  for (const byte of buffer) crc = crcTable[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length, 0);
  const typeBuffer = Buffer.from(type, 'ascii');
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(Buffer.concat([typeBuffer, data])), 0);
  return Buffer.concat([length, typeBuffer, data, crc]);
}

// LVGL RGB565 小端：低字节 GGGBBBBB，高字节 RRRRRGGG；stride 为每行字节数（可能含行尾填充）。
export function rgb565ToPng(width, height, rgb565, stride = width * 2) {
  if (!Number.isInteger(width) || width < 16 || width > MAX_WIDTH) throw new Error('宽度无效');
  if (!Number.isInteger(height) || height < 16 || height > MAX_HEIGHT) throw new Error('高度无效');
  if (!Buffer.isBuffer(rgb565)) throw new Error('像素数据无效');
  if (!Number.isInteger(stride) || stride < width * 2 || stride > MAX_WIDTH * 4) throw new Error('行距无效');
  const expected = stride * height;
  if (rgb565.length !== expected) throw new Error(`数据长度不符：期望 ${expected} 字节，实得 ${rgb565.length}`);
  const raw = Buffer.alloc(height * (1 + width * 3));
  let out = 0;
  for (let y = 0; y < height; y += 1) {
    raw[out] = 0; // filter: none
    out += 1;
    const row = y * stride;
    for (let x = 0; x < width; x += 1) {
      const value = rgb565.readUInt16LE(row + x * 2);
      const r5 = (value >> 11) & 0x1f;
      const g6 = (value >> 5) & 0x3f;
      const b5 = value & 0x1f;
      raw[out] = (r5 << 3) | (r5 >> 2);
      raw[out + 1] = (g6 << 2) | (g6 >> 4);
      raw[out + 2] = (b5 << 3) | (b5 >> 2);
      out += 3;
    }
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 2; // color type: truecolor RGB
  const signature = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  return Buffer.concat([
    signature,
    chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 6 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

export function validateScreenshotParams(query) {
  const width = Number(query.get('w'));
  const height = Number(query.get('h'));
  const stride = query.has('stride') ? Number(query.get('stride')) : width * 2;
  if (!Number.isInteger(width) || width < 16 || width > MAX_WIDTH) throw new Error('宽度无效');
  if (!Number.isInteger(height) || height < 16 || height > MAX_HEIGHT) throw new Error('高度无效');
  if (!Number.isInteger(stride) || stride < width * 2 || stride > MAX_WIDTH * 4) throw new Error('行距无效');
  if ((query.get('fmt') || 'rgb565') !== 'rgb565') throw new Error('仅支持 rgb565 像素格式');
  return { width, height, stride };
}

export function createScreenshotStore(dir) {
  fs.mkdirSync(dir, { recursive: true });
  const sanitize = (deviceId) => String(deviceId).replace(/[^0-9a-f-]/gi, '').slice(0, 40) || 'unknown';
  const prune = () => {
    const files = fs.readdirSync(dir).filter((f) => f.endsWith('.png')).sort();
    for (const stale of files.slice(0, Math.max(0, files.length - KEEP_FILES))) {
      try { fs.unlinkSync(path.join(dir, stale)); } catch { /* 并发清理可忽略 */ }
    }
  };
  return {
    save(deviceId, png) {
      const name = `${Date.now()}-${sanitize(deviceId)}.png`;
      const file = path.join(dir, name);
      fs.writeFileSync(file, png);
      prune();
      return file;
    },
    latest(afterMs) {
      let best = null;
      let bestTime = 0;
      for (const name of fs.readdirSync(dir)) {
        if (!name.endsWith('.png')) continue;
        const stamp = Number(name.split('-')[0]);
        if (!Number.isFinite(stamp) || stamp < afterMs) continue;
        const file = path.join(dir, name);
        let mtime = 0;
        try { mtime = fs.statSync(file).mtimeMs; } catch { continue; }
        if (mtime > bestTime) {
          best = file;
          bestTime = mtime;
        }
      }
      return best;
    },
  };
}

export { MAX_UPLOAD };
