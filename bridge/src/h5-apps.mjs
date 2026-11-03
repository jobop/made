import fs from 'node:fs';
import path from 'node:path';
import { randomBytes } from 'node:crypto';
import { packagesHome, packageRoots } from './config.mjs';
import { readPackageSettings } from './plugin-package.mjs';

// H5 插件（kind=h5）：装到桥接器上的自包含静态网页应用，App/浏览器直接打开页面。
// 板子（ESP32+LVGL）不下发、不参与；页面跑在手机 WebView 里。
//
// 能力协商（安全边界）：
// - 包根 plugin.json 声明 capabilities（白名单子集）；没声明 = 纯静态页，拿不到任何接口；
// - 打开页面时 App 用设备令牌换一张短期 scoped token（/plugins/<id>/token，12 小时），
//   页面带它访问 /h5api/*——每个路由校验 token 对应包声明过的能力，缺能力一律 403；
// - scoped token 与包绑定：换包重装后旧 token 的能力以磁盘上的新 manifest 为准，
//   包被删除则 token 直接失效。

/** 能力白名单：桥接器是唯一裁决点，市场侧（made-market）只做同名单的预检。 */
export const H5_CAPABILITIES = Object.freeze([
  'state.read',      // GET  /h5api/state      精简桥接器状态（任务/会话/助手可用性）
  'tasks.read',      // GET  /h5api/tasks      最近任务列表
  'tasks.write',     // POST /h5api/tasks、/h5api/tasks/<id>/(confirm|cancel)
  'sessions.read',   // GET  /h5api/sessions   会话列表
  'sessions.write',  // POST /h5api/sessions   新建会话
  'settings.read',   // GET  /h5api/settings   读本包配置（密钥类字段自动遮蔽）
]);

const PACKAGE_DIR = /^[a-z0-9][a-z0-9_-]{0,39}$/;
const MAX_MANIFEST = 16 * 1024;
const MAX_STATIC_FILE = 8 * 1024 * 1024;
const TOKEN_TTL_MS = 12 * 60 * 60 * 1000;
const MAX_TOKENS = 64;

const fail = (message) => {
  const error = new Error(message);
  error.preserveMessage = true;
  return error;
};

/** 读取并校验已安装 H5 包的 plugin.json；不合法或不是 h5 包返回 null。 */
export function readH5Manifest(packageDir) {
  const file = path.join(packageDir, 'plugin.json');
  if (!fs.existsSync(file) || !fs.statSync(file).isFile()) return null;
  let raw;
  try {
    if (fs.statSync(file).size > MAX_MANIFEST) return null;
    raw = JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch {
    return null;
  }
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  if (raw.kind !== 'h5') return null;
  if (typeof raw.id !== 'string' || !PACKAGE_DIR.test(raw.id)) return null;
  // manifest 的 id 必须与安装目录名一致（目录名才是路由身份）。
  if (raw.id !== path.basename(packageDir)) return null;
  const id = raw.id;
  let www = 'www/';
  if (raw.www !== undefined) {
    if (typeof raw.www !== 'string' || !raw.www || raw.www.includes('..') || raw.www.includes('\\')
      || path.isAbsolute(raw.www) || !/^[A-Za-z0-9_][A-Za-z0-9_/-]*$/.test(raw.www)) return null;
    www = raw.www.endsWith('/') ? raw.www : `${raw.www}/`;
  }
  let capabilities = [];
  if (raw.capabilities !== undefined) {
    if (!Array.isArray(raw.capabilities) || raw.capabilities.length > 8
      || raw.capabilities.some((name) => !H5_CAPABILITIES.includes(name))) return null;
    capabilities = [...new Set(raw.capabilities)];
  }
  return { id, kind: 'h5', www, capabilities };
}

/** 已安装 H5 包的静态根：packagesHome/plugins/<id>/<www>，目录必须真实存在。 */
export function h5StaticRoot(config, id) {
  if (!PACKAGE_DIR.test(id || '')) return null;
  const packageDir = path.join(packagesHome(config), 'plugins', id);
  const manifest = readH5Manifest(packageDir);
  if (!manifest || manifest.id !== id) return null;
  const root = path.resolve(path.join(packageDir, manifest.www));
  if (root !== packageDir && !root.startsWith(packageDir + path.sep)) return null;
  if (!fs.existsSync(root) || !fs.statSync(root).isDirectory()) return null;
  return { packageDir, manifest, root };
}

/** 按 id 找已安装 h5 包目录；不是合法已装 h5 包返回 null。 */
export function h5PackageDir(config, id) {
  if (!PACKAGE_DIR.test(id || '')) return null;
  const packageDir = path.join(packagesHome(config), 'plugins', id);
  const manifest = readH5Manifest(packageDir);
  return manifest && manifest.id === id ? packageDir : null;
}

const H5_SECRET_KEY = /(key|secret|token|password|passphrase)/i;

/** 插件查找顺序（与 installed.mjs 同源）：内置在前、用户目录在后。 */
function h5PluginsDirs(config) {
  return packageRoots(config).map((root) => path.join(root, 'plugins'));
}

/**
 * 已安装 H5 应用列表（App 小程序面板 / 设置页用）：静态元数据 + 解析后的设置表单。
 * 密钥类配置只出「有没有填」，不出值——页面拿到的也是遮蔽后的（见 /h5api/settings）。
 */
export function listH5Apps(config) {
  const apps = [];
  for (const directory of h5PluginsDirs(config)) {
    if (!fs.existsSync(directory) || !fs.statSync(directory).isDirectory()) continue;
    for (const name of fs.readdirSync(directory).sort()) {
      if (!PACKAGE_DIR.test(name)) continue;
      const packageDir = path.join(directory, name);
      if (!fs.statSync(packageDir).isDirectory()) continue;
      const manifest = readH5Manifest(packageDir);
      if (!manifest || manifest.id !== name) continue;
      if (apps.some((app) => app.id === name)) continue; // 同名包用户目录覆盖内置
      let name2 = name, version = '', summary = '';
      try {
        const market = JSON.parse(fs.readFileSync(path.join(packageDir, 'market.json'), 'utf8'));
        if (typeof market.name === 'string' && market.name.trim()) name2 = market.name.trim();
        if (typeof market.version === 'string') version = market.version;
        if (typeof market.summary === 'string' && market.summary.trim()) summary = market.summary.trim();
      } catch { /* market.json 可选；缺了用 id 当名字 */ }
      const pack = readPackageSettings(packageDir);
      const secretFields = pack.fields.filter((field) => H5_SECRET_KEY.test(field.key)).map((field) => field.key);
      const filledSecrets = Object.fromEntries(secretFields.map((key) => [key, Boolean(String(pack.values[key] || '').trim())]));
      apps.push({
        id: name, name: name2, version, summary, kind: 'h5',
        capabilities: manifest.capabilities,
        settingsSpec: pack.fields,
        settings: pack.values,
        settingsSources: pack.fields.reduce((acc, field) => {
          if (!String(String(field.default || '')).includes('${MADE_LLM_')) return acc;
          const value = String(pack.values[field.key] || '');
          if (value && !value.includes('${MADE_LLM_')) acc[field.key] = 'cloud';
          return acc;
        }, {}),
        secretFields,
        secretFilled: filledSecrets,
        settingsError: pack.error || '',
      });
    }
  }
  return apps;
}

const CONTENT_TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.htm': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif': 'image/gif',
  '.webp': 'image/webp',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.txt': 'text/plain; charset=utf-8',
  '.xml': 'application/xml; charset=utf-8',
  '.wasm': 'application/wasm',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.ttf': 'font/ttf',
  '.otf': 'font/otf',
  '.mp3': 'audio/mpeg',
  '.wav': 'audio/wav',
  '.mp4': 'video/mp4',
  '.webmanifest': 'application/manifest+json',
};

/** H5 静态文件解析：拒绝路径穿越，目录回落 index.html；返回 {file, contentType} 或 null。 */
export function resolveStaticFile(root, relative) {
  const cleaned = String(relative || '').split('/').filter((segment) => segment && segment !== '.');
  if (cleaned.some((segment) => segment === '..' || segment.includes('\\') || segment.includes('%'))) return null;
  let target = path.resolve(path.join(root, ...cleaned));
  if (target !== root && !target.startsWith(root + path.sep)) return null;
  if (fs.existsSync(target) && fs.statSync(target).isDirectory()) target = path.join(target, 'index.html');
  else if (!fs.existsSync(target) && !path.extname(target)) target = `${target}.html`; // 无扩展名路径回落 .html（SPA 友好）
  if (!fs.existsSync(target) || !fs.statSync(target).isFile()) return null;
  if (fs.statSync(target).size > MAX_STATIC_FILE) return null;
  // 符号链接可能指向包目录外：真实路径必须仍落在根内。
  const real = fs.realpathSync(target);
  const realRoot = fs.realpathSync(root);
  if (real !== realRoot && !real.startsWith(realRoot + path.sep)) return null;
  const contentType = CONTENT_TYPES[path.extname(target).toLowerCase()] || 'application/octet-stream';
  return { file: target, contentType };
}

/** 短期 scoped token 存储：包目录 + 能力快照，进程内存态（重启即失效，可接受）。 */
export class H5Tokens {
  constructor() {
    this.tokens = new Map();
  }

  /** 签发：能力以磁盘 manifest 为准（调用方传入），只发白名单交集。 */
  issue(id, capabilities) {
    this.prune();
    if (this.tokens.size >= MAX_TOKENS) {
      const oldest = this.tokens.keys().next().value;
      this.tokens.delete(oldest);
    }
    const token = randomBytes(32).toString('hex');
    this.tokens.set(token, {
      id, capabilities: [...capabilities], expiresAt: Date.now() + TOKEN_TTL_MS,
    });
    return { token, expiresAt: this.tokens.get(token).expiresAt };
  }

  verify(token) {
    const entry = typeof token === 'string' ? this.tokens.get(token) : null;
    if (!entry || entry.expiresAt <= Date.now()) return null;
    return entry;
  }

  revoke(id) {
    for (const [token, entry] of this.tokens) {
      if (entry.id === id) this.tokens.delete(token);
    }
  }

  prune() {
    const now = Date.now();
    for (const [token, entry] of this.tokens) {
      if (entry.expiresAt <= now) this.tokens.delete(token);
    }
  }
}
