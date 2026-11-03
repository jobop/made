import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import zlib from 'node:zlib';
import { randomUUID } from 'node:crypto';
import { packagesHome } from './config.mjs';
import { reloadConfiguredPlugins } from './plugins/hot-reload.mjs';

// 桥接器内的市场安装：拉公开索引 → 下载归档 → sha256 校验 → 解包写入用户包目录
// → reload 即时生效。市场只登记和分发（不执行任何包代码），执行边界仍是桥接器的
// 插件运行时（registry.mjs 的 validatePlugin + 热加载快照回滚）。
//
// 安全边界（这个端点等于"往桥接器进程里装代码"，收紧到最窄）：
// - 索引与归档只来自官方市场域名（MADE_MARKET_SERVER / MADE_MARKET_REGISTRY 可覆盖，
//   供自建市场与本地调试），归档绝对地址必须与索引同源且为 https；
// - 归档 sha256 必须与索引一致，缺字段直接拒绝（比 CLI 更严：桥接器没有人工把关）；
// - 归档条目禁绝对路径/上级跳转/符号链接，数量与总大小设上限（防炸弹）；
// - 安装目录名按包类别白名单校验，落点只能在用户包目录内。

const fail = (message) => {
  const error = new Error(message);
  error.preserveMessage = true;
  return error;
};

const PACKAGE_ID = /^[a-z0-9][a-z0-9_-]{0,39}$/;
const THEME_DIR = /^[A-Za-z0-9_-]{1,64}$/;
const PACKAGE_DIR = /^[a-z0-9][a-z0-9_-]{0,39}$/;
const HEX64 = /^[0-9a-f]{64}$/;

const KIND_TARGET = Object.freeze({ theme: 'themes', control: 'plugins', agent: 'plugins' });
const DEFAULT_SERVER = 'https://market.made-world.link';
const INDEX_TTL_MS = 5 * 60 * 1000;
const INDEX_TIMEOUT_MS = 10_000;
const ARCHIVE_TIMEOUT_MS = 30_000;
const MAX_ARCHIVE_BYTES = 8 * 1024 * 1024;
const MAX_FILES = 512;
const MAX_FILE_BYTES = 8 * 1024 * 1024;
const MAX_TOTAL_BYTES = 32 * 1024 * 1024;

// ---- 索引来源与缓存 ----

export function marketSource() {
  if (process.env.MADE_MARKET_REGISTRY) return process.env.MADE_MARKET_REGISTRY;
  return `${(process.env.MADE_MARKET_SERVER || DEFAULT_SERVER).replace(/\/+$/, '')}/registry/index.json`;
}

/** 站点根：索引固定在 <根>/registry/index.json，归档固定在 <根>/packages/。与 market CLI 同一约定。 */
function siteRoot(source) {
  const trimmed = source.replace(/[?#].*$/, '').replace(/\/+$/, '');
  if (/\/registry\/[^/]+$/.test(trimmed)) return `${trimmed.replace(/\/registry\/[^/]+$/, '')}/`;
  if (/^https?:\/\/[^/]+$/.test(trimmed)) return `${trimmed}/`;
  return trimmed.replace(/[^/]*$/, '');
}

let cache = null; // { source, fetchedAt, raw }

async function fetchJson(url, timeout = INDEX_TIMEOUT_MS) {
  const response = await fetch(url, { redirect: 'follow', signal: AbortSignal.timeout(timeout) });
  if (!response.ok) throw fail(`请求失败：HTTP ${response.status}`);
  return response.json();
}

async function fetchIndex(source, timeout = INDEX_TIMEOUT_MS) {
  if (!/^https?:\/\//.test(source)) {
    if (!fs.existsSync(source)) throw fail(`找不到索引 ${source}`);
    return JSON.parse(fs.readFileSync(source, 'utf8'));
  }
  if (!/^https:\/\//.test(source) && !/^http:\/\/(localhost|127\.0\.0\.1)/.test(source)) {
    throw fail('市场索引必须是 https 地址');
  }
  return fetchJson(source, timeout);
}

/** API 模式：索引只给 packagesEndpoint（分页概要），列表从这里拉。 */
async function fetchApiPackages(source, endpoint) {
  const url = new URL(endpoint, siteRoot(source));
  url.searchParams.set('limit', '200');
  const data = await fetchJson(url.href);
  if (!data || !Array.isArray(data.items)) throw fail('市场列表格式无效');
  return data.items;
}

/**
 * 市场有两种索引形态，都兼容：
 * - 静态索引：registry/index.json 自带 packages[]（仓库快照 / 自建市场 / 离线）。
 * - API 模式：线上官方（服务端 DB 动态索引），index.json 只有 packagesEndpoint，
 *   条目概要缺 sha256 等字段——安装时再从 /api/packages/<id> 取最新版详情补齐。
 */
async function loadMarket(source) {
  const raw = await fetchIndex(source);
  if (raw && Array.isArray(raw.packages)) return { packages: raw.packages, mode: 'static' };
  if (raw && typeof raw.packagesEndpoint === 'string') {
    return { packages: await fetchApiPackages(source, raw.packagesEndpoint), mode: 'api' };
  }
  throw fail('市场索引格式无效');
}

/** 安装条目补齐：概要条目缺 sha256/dir/target 时，从包详情端点取匹配版本（默认最新）。 */
async function resolveEntry(source, entry, mode) {
  if (HEX64.test(entry.sha256 || '') && entry.dir && entry.target) return entry;
  if (mode !== 'api') return entry;
  const versions = await fetchJson(new URL(`api/packages/${entry.id}`, siteRoot(source)).href);
  if (!Array.isArray(versions) || !versions.length) throw fail(`市场里没有包 ${entry.id} 的版本详情`);
  const picked = versions.find((item) => item.version === entry.version) || versions[0];
  return { ...entry, ...picked };
}

/** 公开索引（精简条目 + 已安装标记）。默认 5 分钟缓存，refresh=true 强制拉新。 */
export async function marketIndex(config, { refresh = false } = {}) {
  const source = marketSource();
  if (!refresh && cache && cache.source === source && Date.now() - cache.fetchedAt < INDEX_TTL_MS) {
    return decorate(config, cache.raw, source, cache.fetchedAt);
  }
  let market;
  try {
    market = await loadMarket(source);
  } catch (error) {
    // 有旧缓存就降级用旧的（标记为过期数据），没有才把错误抛出去。
    if (!refresh && cache && cache.source === source) {
      return { ...decorate(config, cache.raw, source, cache.fetchedAt), stale: true };
    }
    throw error.preserveMessage ? error : fail(`市场索引不可用：${error.message}`);
  }
  cache = { source, fetchedAt: Date.now(), raw: market.packages, mode: market.mode };
  return decorate(config, market.packages, source, cache.fetchedAt);
}

/** 精简条目：不透传 download 等分发细节，客户端只做展示与点安装。 */
function decorate(config, packages, source, fetchedAt) {
  const installed = installedDirs(config);
  const list = (packages || []).map((entry) => ({
    id: entry.id,
    kind: entry.kind,
    version: entry.version,
    name: entry.name,
    nameEn: entry.nameEn,
    summary: entry.summary,
    summaryEn: entry.summaryEn,
    author: entry.author,
    license: entry.license,
    tags: entry.tags || [],
    glyph: entry.glyph,
    highlights: entry.highlights || [],
    pluginId: entry.pluginId,
    dir: entry.dir,
    target: entry.target,
    publishedAt: entry.publishedAt,
    installed: installed.has(`${entry.target || KIND_TARGET[entry.kind]}/${entry.dir || entry.id}`),
  }));
  return { source, fetchedAt, packages: list };
}

/** 已安装判定（轻量，按目录名）：用户目录里已有同名包目录即视为已安装。 */
function installedDirs(config) {
  const found = new Set();
  const home = packagesHome(config);
  for (const target of ['plugins', 'themes']) {
    const directory = path.join(home, target);
    if (!fs.existsSync(directory)) continue;
    for (const name of fs.readdirSync(directory)) {
      if (fs.statSync(path.join(directory, name)).isDirectory()) found.add(`${target}/${name}`);
    }
  }
  return found;
}

// ---- 归档下载与解包 ----

function archiveUrl(source, entry) {
  const download = entry.download;
  if (typeof download !== 'string' || !download.trim()) throw fail(`包 ${entry.id} 的索引条目缺少下载地址`);
  const root = siteRoot(source);
  if (/^https?:\/\//i.test(download)) {
    // 绝对地址：必须与索引同源（同 host）且为 https，防索引字段把下载引到别处。
    const target = new URL(download);
    const base = new URL(root);
    if (target.protocol !== 'https:' && !(target.protocol === 'http:' && /^(localhost|127\.0\.0\.1)$/.test(target.hostname))) {
      throw fail(`包 ${entry.id} 的下载地址必须是 https`);
    }
    if (target.host !== base.host) throw fail(`包 ${entry.id} 的下载地址与市场源不同源，已拒绝`);
    return target.href;
  }
  return new URL(download, root).href;
}

async function downloadArchive(source, entry) {
  // 本地文件索引（调试/自建）：归档按 <索引所在站点根>/<download> 从磁盘读，
  // 且必须落在市场目录内，不许索引把下载引到别处。
  if (!/^https?:\/\//.test(source)) {
    const root = path.resolve(siteRoot(source));
    const local = path.resolve(path.join(root, typeof entry.download === 'string' ? entry.download : ''));
    if (local !== root && !local.startsWith(root + path.sep)) throw fail(`包 ${entry.id} 的下载地址越出了市场目录`);
    if (!fs.existsSync(local)) throw fail(`找不到归档 ${entry.download}`);
    return fs.readFileSync(local);
  }
  const url = archiveUrl(source, entry);
  const response = await fetch(url, { redirect: 'follow', signal: AbortSignal.timeout(ARCHIVE_TIMEOUT_MS) });
  if (!response.ok) throw fail(`归档下载失败：HTTP ${response.status}`);
  const data = Buffer.from(await response.arrayBuffer());
  if (data.length > MAX_ARCHIVE_BYTES) throw fail(`归档 ${data.length} 字节，超过 ${MAX_ARCHIVE_BYTES} 上限`);
  return data;
}

// ustar 解包：与 market CLI（tools/lib/tar.mjs）同一格式约定——市场打包器只产
// 普通文件 + 短文件名的确定性归档，这里只实现读取所需的子集，并逐条目校验头部校验和。
const BLOCK = 512;

const readOctal = (buf, offset, length) => {
  const text = buf.toString('latin1', offset, offset + length).replace(/\0.*$/, '').trim();
  return text ? parseInt(text, 8) : 0;
};

function extractTar(buffer) {
  if (!Buffer.isBuffer(buffer) || buffer.length % BLOCK !== 0) throw fail('归档不是合法的 tar：长度不是 512 的整数倍');
  const entries = [];
  let offset = 0;
  while (offset + BLOCK <= buffer.length) {
    const head = buffer.subarray(offset, offset + BLOCK);
    if (head.every((byte) => byte === 0)) break;
    const name = head.toString('utf8', 0, 100).replace(/\0.*$/, '');
    const size = readOctal(head, 124, 12);
    const type = head.toString('latin1', 156, 157);
    const stored = readOctal(head, 148, 8);
    let sum = 0;
    for (let i = 0; i < BLOCK; i++) sum += i >= 148 && i < 156 ? 32 : head[i];
    if (sum !== stored) throw fail(`归档条目校验和错误：${name || '(未命名)'}`);
    if (size < 0 || offset + BLOCK + size > buffer.length) throw fail(`归档条目长度越界：${name}`);
    const data = buffer.subarray(offset + BLOCK, offset + BLOCK + size);
    if (type === '0' || type === '\0' || type === '') {
      if (name) entries.push({ name, data: Buffer.from(data) });
    } else if (type !== '5' && type !== 'x' && type !== 'g') {
      // 只接受普通文件（目录/占位头跳过），符号链接等一律拒绝。
      throw fail(`归档包含不支持的条目类型 ${type}：${name}`);
    }
    offset += BLOCK + Math.ceil(size / BLOCK) * BLOCK;
  }
  return entries;
}

function unpack(archive, entry) {
  const digest = crypto.createHash('sha256').update(archive).digest('hex');
  if (!HEX64.test(entry.sha256 || '')) throw fail(`包 ${entry.id} 的索引条目缺少 sha256，拒绝安装`);
  if (digest !== entry.sha256) throw fail(`${entry.id} 的 sha256 不匹配：期望 ${entry.sha256}，实际 ${digest}`);
  let files;
  try {
    files = extractTar(zlib.gunzipSync(archive));
  } catch (error) {
    throw error.preserveMessage ? error : fail(`归档解压失败：${error.message}`);
  }
  if (!files.length) throw fail('归档是空的');
  if (files.length > MAX_FILES) throw fail(`归档包含 ${files.length} 个文件，超过 ${MAX_FILES} 上限`);
  let total = 0;
  let hasEntryFile = false;
  const expectedEntry = entry.kind === 'theme' ? 'theme.json' : null;
  for (const file of files) {
    if (file.name.includes('..') || path.isAbsolute(file.name) || file.name.includes('\\')) {
      throw fail(`归档条目路径不安全：${file.name}`);
    }
    if (file.data.length > MAX_FILE_BYTES) throw fail(`归档条目 ${file.name} 超过单文件上限`);
    total += file.data.length;
    if (total > MAX_TOTAL_BYTES) throw fail(`归档解包后超过 ${MAX_TOTAL_BYTES} 字节上限`);
    if (expectedEntry ? file.name === expectedEntry : file.name === 'plugin.mjs' || file.name === 'plugin.js') hasEntryFile = true;
  }
  if (!hasEntryFile) {
    throw fail(expectedEntry ? `归档缺少 ${expectedEntry}` : '归档缺少 plugin.mjs / plugin.js');
  }
  return files;
}

// ---- 落盘与生效 ----

function checkInstallDir(kind, dir) {
  if (typeof dir !== 'string' || !(kind === 'theme' ? THEME_DIR.test(dir) : PACKAGE_DIR.test(dir))) {
    throw fail(`安装目录名 ${dir} 不符合 ${kind === 'theme' ? '主题' : '插件'}命名规则`);
  }
  return dir;
}

function writeTree(target, files) {
  fs.mkdirSync(target, { recursive: true });
  for (const file of files) {
    const dest = path.join(target, file.name);
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.writeFileSync(dest, file.data);
  }
}

/**
 * 安装一个市场包到用户包目录并即时生效。
 * - 覆盖安装：旧目录整体改名备份 → 换上新目录 → 回拷用户的 settings.json → 删备份；
 *   配置是用户数据，升级重装不能丢。
 * - 生效失败（新包加载/校验不过）回滚到旧目录再 reload，失败原因原样抛出。
 */
export async function installPackage(config, { id = '' } = {}) {
  if (!PACKAGE_ID.test(id || '')) throw fail('包 ID 无效');
  // 安装是低频操作，直连市场拿原始条目（含 download/sha256），不走
  // marketIndex 的精简缓存——那份缓存是给展示用的，不含分发字段。
  const source = marketSource();
  const market = await loadMarket(source);
  const found = market.packages.find((item) => item.id === id);
  if (!found) throw fail(`市场里没有包 ${id}`);
  const entry = await resolveEntry(source, found, market.mode);
  const kind = entry.kind;
  if (!KIND_TARGET[kind]) throw fail(`不支持的包类别 ${kind}`);
  const dir = checkInstallDir(kind, entry.dir);

  const archive = await downloadArchive(source, entry);
  const files = unpack(archive, entry);

  const home = packagesHome(config);
  const target = path.join(home, KIND_TARGET[kind], dir);
  const stagingRoot = path.join(home, '.install-tmp');
  fs.mkdirSync(stagingRoot, { recursive: true });
  const staging = path.join(stagingRoot, `${dir}-${randomUUID()}`);
  const backup = path.join(stagingRoot, `${dir}-old-${randomUUID()}`);
  const hadOld = fs.existsSync(target);
  try {
    writeTree(staging, files);
    fs.mkdirSync(path.dirname(target), { recursive: true }); // themes/ 或 plugins/ 可能还不存在
    if (hadOld) fs.renameSync(target, backup);
    fs.renameSync(staging, target);
    if (hadOld) {
      // 用户配置跟随升级：旧 settings.json 拷进新目录后备份即弃。
      const saved = path.join(backup, 'settings.json');
      if (fs.existsSync(saved)) fs.copyFileSync(saved, path.join(target, 'settings.json'));
      fs.rmSync(backup, { recursive: true, force: true });
    }
    if (kind !== 'theme') {
      try {
        await reloadConfiguredPlugins(config);
      } catch (error) {
        // 新包加载失败：删新目录、还原旧目录，再恢复一次运行时状态。
        fs.rmSync(target, { recursive: true, force: true });
        if (hadOld) fs.renameSync(backup, target);
        await reloadConfiguredPlugins(config).catch(() => {});
        throw error.preserveMessage ? error : fail(`插件加载失败：${error.message}`);
      }
    }
  } finally {
    fs.rmSync(staging, { recursive: true, force: true });
    if (fs.existsSync(backup)) fs.rmSync(backup, { recursive: true, force: true });
  }
  return {
    installed: {
      id: entry.id,
      kind,
      version: entry.version,
      name: entry.name,
      target: KIND_TARGET[kind],
      dir,
      reloaded: kind !== 'theme',
    },
  };
}
