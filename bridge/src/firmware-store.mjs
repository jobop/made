import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

const MAX_BYTES = 8 * 1024 * 1024;
const VERSION_RE = /^[0-9A-Za-z][0-9A-Za-z._-]{0,31}$/;
const ID_RE = /^[0-9a-f]{12}$/;

function emptyManifest() {
  return { otaUrl: '', currentId: '', items: [] };
}

function readManifest(file) {
  try {
    const parsed = JSON.parse(fs.readFileSync(file, 'utf8'));
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) return emptyManifest();
    return {
      otaUrl: typeof parsed.otaUrl === 'string' ? parsed.otaUrl : '',
      currentId: typeof parsed.currentId === 'string' ? parsed.currentId : '',
      items: Array.isArray(parsed.items) ? parsed.items.filter((item) => item && typeof item === 'object') : [],
    };
  } catch {
    return emptyManifest();
  }
}

export const BRIDGE_FIRMWARE_PATH = '/device/firmware/current';

export function validateOtaUrl(value) {
  const text = String(value || '').trim();
  if (!text) return '';
  if (text === BRIDGE_FIRMWARE_PATH) return text;
  let url;
  try { url = new URL(text); } catch { throw new Error('固件地址须是 http(s) URL，或本机路径 /device/firmware/current'); }
  if (url.protocol !== 'http:' && url.protocol !== 'https:') throw new Error('固件地址须是 http(s) URL，或本机路径 /device/firmware/current');
  if (url.username || url.password) throw new Error('固件地址不能带账号密码');
  return url.toString();
}

export function createFirmwareStore(directory) {
  const root = path.resolve(directory);
  const manifestPath = path.join(root, 'manifest.json');
  fs.mkdirSync(root, { recursive: true, mode: 0o700 });

  const load = () => readManifest(manifestPath);
  const save = (manifest) => {
    const temp = `${manifestPath}.${process.pid}.tmp`;
    fs.writeFileSync(temp, JSON.stringify(manifest, null, 2), { mode: 0o600 });
    fs.renameSync(temp, manifestPath);
  };
  const binPath = (id) => path.join(root, `${id}.bin`);

  function publicItem(item) {
    return {
      id: item.id, version: item.version, size: item.size, sha256: item.sha256,
      createdAt: item.createdAt, notes: item.notes || '',
      current: item.id === load().currentId,
    };
  }

  function currentItem(manifest = load()) {
    return manifest.items.find((item) => item.id === manifest.currentId) || null;
  }

  return {
    summary() {
      const manifest = load();
      const current = currentItem(manifest);
      return {
        otaUrl: manifest.otaUrl,
        current: current ? publicItem(current) : null,
        items: manifest.items.map(publicItem),
      };
    },
    setOtaUrl(value) {
      const manifest = load();
      manifest.otaUrl = validateOtaUrl(value);
      save(manifest);
      return this.summary();
    },
    importBuffer({ version, bytes, notes = '' }) {
      const name = String(version || '').trim();
      if (!VERSION_RE.test(name)) throw new Error('版本号需为 1–32 位字母、数字、点或横线');
      if (!Buffer.isBuffer(bytes) || bytes.length < 16 || bytes.length > MAX_BYTES) {
        throw new Error('固件文件需在 16 字节到 8MB 之间');
      }
      const id = crypto.randomBytes(6).toString('hex');
      const sha256 = crypto.createHash('sha256').update(bytes).digest('hex');
      const file = binPath(id);
      fs.writeFileSync(file, bytes, { mode: 0o600 });
      const manifest = load();
      manifest.items.unshift({
        id, version: name, size: bytes.length, sha256,
        createdAt: new Date().toISOString(), notes: String(notes || '').slice(0, 120),
      });
      manifest.currentId = id;
      save(manifest);
      return this.summary();
    },
    async syncFromUrl(fetchImpl = globalThis.fetch) {
      const manifest = load();
      const target = validateOtaUrl(manifest.otaUrl);
      if (!target) throw new Error('请先填写固件地址');
      if (!target.startsWith('http')) throw new Error('本机地址表示码得来这台桥接器取包，请先上传固件');
      const response = await fetchImpl(target);
      if (!response.ok) throw new Error(`拉取固件失败：HTTP ${response.status}`);
      const bytes = Buffer.from(await response.arrayBuffer());
      let version = 'remote';
      try {
        const leaf = decodeURIComponent(new URL(target).pathname.split('/').filter(Boolean).pop() || '');
        version = leaf.replace(/\.bin$/i, '') || version;
      } catch { /* 用默认版本名 */ }
      if (!VERSION_RE.test(version)) version = `remote-${new Date().toISOString().slice(0, 10).replace(/-/g, '')}`;
      return this.importBuffer({ version, bytes, notes: target });
    },
    remove(id) {
      if (!ID_RE.test(id || '')) throw new Error('固件不存在');
      const manifest = load();
      const item = manifest.items.find((entry) => entry.id === id);
      if (!item) throw new Error('固件不存在');
      manifest.items = manifest.items.filter((entry) => entry.id !== id);
      if (manifest.currentId === id) manifest.currentId = manifest.items[0]?.id || '';
      save(manifest);
      fs.rmSync(binPath(id), { force: true });
      return this.summary();
    },
    setCurrent(id) {
      if (!ID_RE.test(id || '')) throw new Error('固件不存在');
      const manifest = load();
      if (!manifest.items.some((entry) => entry.id === id)) throw new Error('固件不存在');
      manifest.currentId = id;
      save(manifest);
      return this.summary();
    },
    deviceOffer() {
      const manifest = load();
      const current = currentItem(manifest);
      if (!current) {
        return { version: '', size: 0, sha256: '', url: manifest.otaUrl, path: '' };
      }
      return {
        version: current.version,
        size: current.size,
        sha256: current.sha256,
        url: manifest.otaUrl,
        path: '/device/firmware/current',
      };
    },
    readCurrent(offset, length) {
      const item = currentItem();
      if (!item) return null;
      const file = binPath(item.id);
      if (!fs.existsSync(file)) return null;
      const start = Number.isInteger(offset) && offset >= 0 ? offset : 0;
      const want = Number.isInteger(length) && length > 0 ? Math.min(length, 200 * 1024) : 128 * 1024;
      const fd = fs.openSync(file, 'r');
      try {
        const buffer = Buffer.alloc(want);
        const read = fs.readSync(fd, buffer, 0, want, start);
        return { item, offset: start, bytes: buffer.subarray(0, read) };
      } finally {
        fs.closeSync(fd);
      }
    },
  };
}
