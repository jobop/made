import fs from 'node:fs';
import path from 'node:path';
import { randomBytes } from 'node:crypto';
import { isIP, createServer } from 'node:net';
import { spawn } from 'node:child_process';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { OrayManager } from './oray.mjs';

const QUICK_TUNNEL_URL = /https:\/\/[a-z0-9-]+\.trycloudflare\.com(?![a-z0-9.-])/i;
const NAMED_CONNECTED = /Registered tunnel connection/i;
const STARTUP_TIMEOUT_MS = 30_000;
const TAILSCALE_STARTUP_TIMEOUT_MS = 120_000;
const execFileAsync = promisify(execFile);
const TAILSCALE_TARGET_PORT = 443;

async function freeLoopbackPort() {
  return await new Promise((resolve, reject) => {
    const server = createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const port = server.address().port;
      server.close((error) => error ? reject(error) : resolve(port));
    });
  });
}

function validNgrokToken(value) {
  if (typeof value !== 'string' || value.length < 20 || value.length > 8192 ||
      !/^[A-Za-z0-9._-]+$/.test(value)) throw new Error('ngrok Authtoken 格式无效');
  return value;
}

export function inspectNgrokEndpointStatus(raw, devicePort, configuredUrl = null) {
  const entries = (typeof raw === 'string' ? JSON.parse(raw) : raw)?.endpoints;
  if (!Array.isArray(entries)) return null;
  for (const entry of entries) {
    if (!tailscaleTargetMatches(entry?.upstream?.url, devicePort) || typeof entry.url !== 'string') continue;
    try {
      const url = namedUrl(entry.url);
      if (!configuredUrl || url === configuredUrl) return url;
    } catch { /* Never advertise a non-HTTPS or malformed endpoint. */ }
  }
  return null;
}

function inspectLegacyNgrokTunnelStatus(raw, devicePort, configuredUrl = null) {
  const entries = (typeof raw === 'string' ? JSON.parse(raw) : raw)?.tunnels;
  if (!Array.isArray(entries)) return null;
  for (const entry of entries) {
    const target = entry?.config?.addr;
    const validTarget = target === `127.0.0.1:${devicePort}` || tailscaleTargetMatches(target, devicePort);
    if (!validTarget || typeof entry.public_url !== 'string') continue;
    try {
      const url = namedUrl(entry.public_url);
      if (!configuredUrl || url === configuredUrl) return url;
    } catch { /* Do not advertise invalid or HTTP addresses. */ }
  }
  return null;
}

async function runTailscaleCommand(command, args) {
  const { stdout } = await execFileAsync(command, args, { timeout: 7000, maxBuffer: 128 * 1024 });
  return stdout;
}

function statusSections(config) {
  if (!config || typeof config !== 'object' || Array.isArray(config)) return [];
  const foreground = config.Foreground && typeof config.Foreground === 'object' ?
    Object.values(config.Foreground) : [];
  return [config, ...foreground.filter((entry) => entry && typeof entry === 'object')];
}

function tailscaleHostPort(value) {
  const match = /^([a-z0-9-]+(?:\.[a-z0-9-]+)*\.ts\.net):443$/i.exec(value);
  return match ? match[1].toLowerCase() : null;
}

function tailscaleTargetMatches(value, devicePort) {
  try {
    const url = new URL(value);
    return url.protocol === 'http:' && url.hostname === '127.0.0.1' &&
      url.port === String(devicePort) && url.pathname === '/' && !url.search && !url.hash;
  } catch { return false; }
}

export function inspectTailscaleFunnelStatus(raw, devicePort) {
  const config = typeof raw === 'string' ? JSON.parse(raw) : raw;
  let url = null;
  let portOccupied = false;
  for (const section of statusSections(config)) {
    if (section.TCP?.[String(TAILSCALE_TARGET_PORT)] ||
        Object.keys(section.Web || {}).some((key) => key.endsWith(`:${TAILSCALE_TARGET_PORT}`))) {
      portOccupied = true;
    }
    for (const [hostPort, allowed] of Object.entries(section.AllowFunnel || {})) {
      if (!allowed || !hostPort.endsWith(`:${TAILSCALE_TARGET_PORT}`)) continue;
      portOccupied = true;
      const hostname = tailscaleHostPort(hostPort);
      const proxy = section.Web?.[hostPort]?.Handlers?.['/']?.Proxy;
      if (hostname && section.TCP?.[String(TAILSCALE_TARGET_PORT)]?.HTTPS === true &&
          tailscaleTargetMatches(proxy, devicePort)) url = `https://${hostname}`;
    }
  }
  return { url, portOccupied };
}

function writePrivate(file, value) {
  fs.mkdirSync(path.dirname(file), { recursive: true, mode: 0o700 });
  const temporary = `${file}.${process.pid}.${randomBytes(4).toString('hex')}.tmp`;
  fs.writeFileSync(temporary, value, { mode: 0o600 });
  fs.renameSync(temporary, file);
  fs.chmodSync(file, 0o600);
}

function namedUrl(value) {
  if (typeof value !== 'string' || value.length > 253) throw new Error('固定地址必须是 HTTPS 公网域名');
  let parsed;
  try { parsed = new URL(value.trim()); }
  catch { throw new Error('固定地址必须是 HTTPS 公网域名'); }
  const hostname = parsed.hostname.toLowerCase();
  const validHost = hostname.length <= 253 && hostname.split('.').length >= 2 &&
    hostname.split('.').every((label) => /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(label));
  if (parsed.protocol !== 'https:' || !validHost || isIP(hostname) || parsed.username || parsed.password ||
      parsed.port || parsed.pathname !== '/' || parsed.search || parsed.hash) {
    throw new Error('固定地址须为 HTTPS 域名，不含路径、端口或参数');
  }
  return `https://${hostname}`;
}

function validateToken(value) {
  if (typeof value !== 'string' || value.length < 30 || value.length > 8192 || !/^[A-Za-z0-9+/_=.\-]+$/.test(value)) {
    throw new Error('Cloudflare Tunnel 令牌格式无效');
  }
  return value;
}

// Only the authenticated device API is tunnelled. The desktop panel is a
// different server bound to loopback and is never passed to a tunnel provider.
export class TunnelManager {
  constructor({ devicePort, stateDir = null, command = 'cloudflared', tailscaleCommand = 'tailscale',
    ngrokCommand = 'ngrok', spawnProcess = spawn, runCommand = runTailscaleCommand,
    fetchImpl = fetch, pickNgrokPort = freeLoopbackPort, startupTimeoutMs = null,
    bridgeId = null, orayManager = null }) {
    this.devicePort = devicePort;
    this.command = command;
    this.tailscaleCommand = tailscaleCommand;
    this.ngrokCommand = ngrokCommand;
    this.spawnProcess = spawnProcess;
    this.runCommand = runCommand;
    this.fetchImpl = fetchImpl;
    this.pickNgrokPort = pickNgrokPort;
    this.startupTimeoutMs = startupTimeoutMs ?? STARTUP_TIMEOUT_MS;
    this.tailscaleStartupTimeoutMs = startupTimeoutMs ?? TAILSCALE_STARTUP_TIMEOUT_MS;
    this.settingsFile = stateDir ? path.join(stateDir, 'tunnel-settings.json') : null;
    this.tokenFile = stateDir ? path.join(stateDir, 'tunnel-token') : null;
    this.ngrokTokenFile = stateDir ? path.join(stateDir, 'ngrok-token') : null;
    this.ngrokRuntimeFile = stateDir ? path.join(stateDir, 'ngrok-runtime.yml') : null;
    this.oray = orayManager || new OrayManager({ devicePort, stateDir, bridgeId, fetchImpl });
    this.mode = 'quick';
    this.configuredUrl = null;
    this.ngrokConfiguredUrl = null;
    if (this.settingsFile) {
      try {
        const saved = JSON.parse(fs.readFileSync(this.settingsFile, 'utf8'));
        if (['named', 'tailscale', 'ngrok', 'oray'].includes(saved.mode)) this.mode = saved.mode;
        if (saved.configuredUrl) this.configuredUrl = namedUrl(saved.configuredUrl);
        if (saved.ngrokConfiguredUrl) this.ngrokConfiguredUrl = namedUrl(saved.ngrokConfiguredUrl);
      } catch { /* Damaged settings never authorize a public tunnel. */ }
    }
    this.process = null;
    this.status = 'off';
    this.url = null;
    this.error = '';
    this.generation = 0;
    this.timer = null;
    this.pollTimer = null;
    this.polling = false;
    this.cleanupAvailable = false;
    this.ngrokApiPort = null;
    this.stopping = false;
  }

  get tokenConfigured() {
    try { return Boolean(this.tokenFile && validateToken(fs.readFileSync(this.tokenFile, 'utf8').trim())); }
    catch { return false; }
  }

  get ngrokTokenConfigured() {
    try { return Boolean(this.ngrokTokenFile && validNgrokToken(fs.readFileSync(this.ngrokTokenFile, 'utf8').trim())); }
    catch { return false; }
  }

  summary() {
    if (this.mode === 'oray') {
      const oray = this.oray.summary();
      return { ...oray, mode: this.mode, running: oray.mappingEnabled,
        cleanupAvailable: oray.status === 'error' && Boolean(oray.domain),
        configuredUrl: oray.domain ? `https://${oray.domain}` : null,
        tokenConfigured: false, ngrokConfiguredUrl: this.ngrokConfiguredUrl,
        ngrokTokenConfigured: this.ngrokTokenConfigured };
    }
    return { status: this.status, url: this.url, error: this.error, mode: this.mode,
      running: Boolean(this.process) || this.stopping, cleanupAvailable: this.cleanupAvailable,
      configuredUrl: this.configuredUrl, tokenConfigured: this.tokenConfigured,
      ngrokConfiguredUrl: this.ngrokConfiguredUrl, ngrokTokenConfigured: this.ngrokTokenConfigured };
  }

  configure({ mode, url, token, clearToken } = {}) {
    if (this.process || this.stopping || this.cleanupAvailable || this.status === 'starting' ||
        this.status === 'online' || this.oray.summary().mappingEnabled) {
      throw new Error('请先关闭当前穿透，再修改设置');
    }
    if (!['quick', 'named', 'tailscale', 'ngrok', 'oray'].includes(mode)) throw new Error('请选择有效的公网接入方式');
    if (clearToken !== undefined && typeof clearToken !== 'boolean') throw new Error('清除令牌参数无效');
    if (clearToken && token) throw new Error('不能同时设置和清除令牌');
    const configuredUrl = mode === 'named' && url ? namedUrl(url) : this.configuredUrl;
    const ngrokConfiguredUrl = mode === 'ngrok' ? (url ? namedUrl(url) : null) : this.ngrokConfiguredUrl;
    const newToken = token ? (mode === 'ngrok' ? validNgrokToken(token) :
      mode === 'oray' ? token : validateToken(token)) : null;
    if (mode === 'named' && (!configuredUrl || (!newToken && (clearToken || !this.tokenConfigured)))) {
      throw new Error('固定地址模式需要 HTTPS 域名和 Cloudflare Tunnel 令牌');
    }
    if (mode === 'ngrok' && !newToken && (clearToken || !this.ngrokTokenConfigured)) {
      throw new Error('ngrok 需要先填写 Authtoken');
    }
    if (mode === 'oray' && !url && !this.oray.summary().domain) {
      throw new Error('请填写花生壳 HTTPS 映射域名');
    }
    if (mode === 'oray' && !newToken && (clearToken || !this.oray.apiKeyConfigured)) {
      throw new Error('请填写花生壳 API Key');
    }
    if (!this.settingsFile || !this.tokenFile) throw new Error('桥接器未配置状态目录');
    if (mode === 'oray') {
      this.oray.configure({ domain: url || undefined, apiKey: token || undefined, clearApiKey: clearToken });
    } else {
      const selectedTokenFile = mode === 'ngrok' ? this.ngrokTokenFile : this.tokenFile;
      if (newToken) writePrivate(selectedTokenFile, newToken);
      else if (clearToken) fs.rmSync(selectedTokenFile, { force: true });
    }
    writePrivate(this.settingsFile, JSON.stringify({ mode, configuredUrl, ngrokConfiguredUrl }, null, 2));
    this.mode = mode;
    this.configuredUrl = configuredUrl;
    this.ngrokConfiguredUrl = ngrokConfiguredUrl;
    this.status = 'off';
    this.url = null;
    this.error = '';
    this.cleanupAvailable = false;
    return this.summary();
  }

  async tailscaleStatus() {
    const raw = await this.runCommand(this.tailscaleCommand, ['funnel', 'status', '--json']);
    return inspectTailscaleFunnelStatus(raw, this.devicePort);
  }

  async ngrokStatus() {
    if (!this.ngrokApiPort) return null;
    const response = await this.fetchImpl(`http://127.0.0.1:${this.ngrokApiPort}/api/endpoints`,
      { signal: AbortSignal.timeout(2500) });
    if (response.status === 404) {
      const legacy = await this.fetchImpl(`http://127.0.0.1:${this.ngrokApiPort}/api/tunnels`,
        { signal: AbortSignal.timeout(2500) });
      if (!legacy.ok) throw new Error('ngrok local API unavailable');
      return inspectLegacyNgrokTunnelStatus(await legacy.json(), this.devicePort, this.ngrokConfiguredUrl);
    }
    if (!response.ok) throw new Error('ngrok local API unavailable');
    return inspectNgrokEndpointStatus(await response.json(), this.devicePort, this.ngrokConfiguredUrl);
  }

  async start() {
    if (this.mode === 'oray') {
      await this.oray.start();
      return this.summary();
    }
    if (this.stopping) return this.summary();
    if (this.process) return this.summary();
    if (this.status === 'starting') return this.summary();
    if (this.cleanupAvailable) return this.summary();
    if (this.mode === 'named' && (!this.configuredUrl || !this.tokenConfigured)) {
      this.status = 'error';
      this.error = '请先保存固定域名和 Cloudflare Tunnel 令牌。';
      return this.summary();
    }
    if (this.mode === 'ngrok' && !this.ngrokTokenConfigured) {
      this.status = 'error';
      this.error = '请先在电脑端保存 ngrok Authtoken。';
      return this.summary();
    }
    this.generation += 1;
    const generation = this.generation;
    this.status = 'starting';
    this.url = null;
    this.error = '';
    if (this.mode === 'tailscale') {
      try {
        const existing = await this.tailscaleStatus();
        if (generation !== this.generation) return this.summary();
        if (existing.portOccupied) {
          this.cleanupAvailable = Boolean(existing.url);
          this.fail(existing.url ?
            '检测到指向本机码得接口的 Tailscale 公网路由。请点击“清理残留路由”后重试。' :
            'Tailscale 的 443 端口已有服务；请检查 tailscale funnel status，勿覆盖原有 Serve/Funnel。');
          return this.summary();
        }
      } catch (error) {
        if (generation !== this.generation) return this.summary();
        this.fail(error?.code === 'ENOENT' ? '未安装 tailscale，请先在电脑上安装并登录。' :
          '无法读取 Tailscale 状态，请检查登录、运行状态与 Funnel 权限。');
        return this.summary();
      }
    }
    if (this.mode === 'ngrok') {
      try {
        this.ngrokApiPort = await this.pickNgrokPort();
        if (!Number.isInteger(this.ngrokApiPort) || this.ngrokApiPort < 1024 || this.ngrokApiPort > 65535) {
          throw new Error('Invalid local API port');
        }
        if (generation !== this.generation) return this.summary();
        const token = validNgrokToken(fs.readFileSync(this.ngrokTokenFile, 'utf8').trim());
        writePrivate(this.ngrokRuntimeFile,
          `version: 3\nagent:\n  authtoken: ${JSON.stringify(token)}\n  web_addr: 127.0.0.1:${this.ngrokApiPort}\n  inspect_db_size: -1\n  remote_management: false\n`);
      } catch {
        if (generation === this.generation) this.fail('无法准备 ngrok 本机配置，请检查状态目录。');
        return this.summary();
      }
    }
    const args = this.mode === 'named' ? ['tunnel', 'run', '--token-file', this.tokenFile] :
      this.mode === 'tailscale' ? ['funnel', '--https=443', `http://127.0.0.1:${this.devicePort}`] :
        this.mode === 'ngrok' ? ['http', `http://127.0.0.1:${this.devicePort}`, '--config', this.ngrokRuntimeFile,
          '--inspect=false', ...(this.ngrokConfiguredUrl ? ['--url', this.ngrokConfiguredUrl] : [])] :
        ['tunnel', '--url', `http://127.0.0.1:${this.devicePort}`];
    let child;
    try {
      child = this.spawnProcess(this.mode === 'tailscale' ? this.tailscaleCommand :
        this.mode === 'ngrok' ? this.ngrokCommand : this.command,
        args, { stdio: ['ignore', 'pipe', 'pipe'] });
    } catch (error) {
      this.status = 'error';
      this.error = error?.code === 'ENOENT' ? `未安装 ${this.mode === 'tailscale' ? 'tailscale' : this.mode === 'ngrok' ? 'ngrok' : 'cloudflared'}，请先在电脑上安装。` : '无法启动内网穿透。';
      this.removeNgrokRuntime();
      return this.summary();
    }
    this.process = child;
    let logTail = '';
    const readLog = (chunk) => {
      if (generation !== this.generation) return;
      logTail = (logTail + chunk.toString('utf8')).slice(-4096);
      if (this.mode === 'tailscale' || this.mode === 'ngrok') return;
      const connected = this.mode === 'named' ? NAMED_CONNECTED.test(logTail) : QUICK_TUNNEL_URL.exec(logTail);
      if (connected) {
        this.url = this.mode === 'named' ? this.configuredUrl : connected[0];
        this.status = 'online';
        this.error = '';
        if (this.timer) clearTimeout(this.timer);
        this.timer = null;
      }
    };
    child.stdout?.on('data', readLog);
    child.stderr?.on('data', readLog);
    child.once('error', (error) => {
      if (generation !== this.generation) return;
      const message = error?.code === 'ENOENT' ?
        `未安装 ${this.mode === 'tailscale' ? 'tailscale' : this.mode === 'ngrok' ? 'ngrok' : 'cloudflared'}，请先在电脑上安装。` : '内网穿透启动失败。';
      if (this.mode === 'tailscale' || this.mode === 'ngrok') void this.stopWithError(message);
      else this.fail(message);
    });
    child.once('exit', () => {
      if (generation !== this.generation) return;
      const message = this.url ? '内网穿透连接已断开，请重新开启。' :
        this.mode === 'tailscale' ? 'Tailscale Funnel 未能建立，请检查登录和 Funnel 权限。' :
          this.mode === 'ngrok' ? 'ngrok 未能建立，请检查 Authtoken、域名与网络连接。' :
          '未能建立内网穿透，请检查 cloudflared 与网络连接。';
      if (this.mode === 'tailscale' || this.mode === 'ngrok') void this.stopWithError(message);
      else this.fail(message);
    });
    if (this.mode === 'tailscale') {
      this.pollTimer = setInterval(async () => {
        if (generation !== this.generation || this.polling) return;
        this.polling = true;
        try {
          const observed = await this.tailscaleStatus();
          if (generation !== this.generation) return;
          if (observed.url) {
            this.url = observed.url;
            this.status = 'online';
            this.error = '';
            if (this.timer) clearTimeout(this.timer);
            this.timer = null;
          } else if (this.status === 'online') {
            this.status = 'error';
            this.url = null;
            this.error = 'Tailscale 公网路由已失效，请关闭后重新开启。';
          }
        } catch {
          if (generation === this.generation && this.status === 'online') {
            this.status = 'error';
            this.url = null;
            this.error = '无法核实 Tailscale 公网路由，请检查客户端状态。';
          }
        } finally { this.polling = false; }
      }, 1000);
      this.pollTimer.unref?.();
    }
    if (this.mode === 'ngrok') {
      this.pollTimer = setInterval(async () => {
        if (generation !== this.generation || this.polling) return;
        this.polling = true;
        try {
          const observed = await this.ngrokStatus();
          if (generation !== this.generation) return;
          if (observed) {
            this.url = observed;
            this.status = 'online';
            this.error = '';
            if (this.timer) clearTimeout(this.timer);
            this.timer = null;
          } else if (this.status === 'online') {
            void this.stopWithError('ngrok 公网路由已失效或转发目标不符。');
          }
        } catch {
          if (generation === this.generation && this.status === 'online') {
            void this.stopWithError('无法核实 ngrok 公网路由，已关闭穿透。');
          }
        } finally { this.polling = false; }
      }, 1000);
      this.pollTimer.unref?.();
    }
    this.timer = setTimeout(() => {
      if (generation !== this.generation || this.url) return;
      if (this.mode === 'tailscale' || this.mode === 'ngrok') {
        void this.stopWithError(this.mode === 'ngrok' ? 'ngrok 启动超时，请检查 Authtoken、域名与网络连接。' :
          'Tailscale Funnel 启动超时，请检查登录和 Funnel 权限。');
      } else {
        this.fail('内网穿透连接超时，请检查 cloudflared、令牌与网络连接。');
        child.kill('SIGTERM');
      }
    }, this.mode === 'tailscale' ? this.tailscaleStartupTimeoutMs : this.startupTimeoutMs);
    this.timer.unref?.();
    return this.summary();
  }

  async refresh() {
    if (this.mode === 'oray' && this.oray.apiKeyConfigured) await this.oray.refresh();
    return this.summary();
  }

  fail(message) {
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    if (this.pollTimer) clearInterval(this.pollTimer);
    this.pollTimer = null;
    this.process = null;
    this.removeNgrokRuntime();
    this.status = 'error';
    this.url = null;
    this.error = message;
  }

  removeNgrokRuntime() {
    if (this.mode !== 'ngrok') return;
    if (this.ngrokRuntimeFile) fs.rmSync(this.ngrokRuntimeFile, { force: true });
    this.ngrokApiPort = null;
  }

  async stopWithError(message) {
    await this.stop();
    if (this.status === 'off') {
      this.status = 'error';
      this.error = message;
    }
    return this.summary();
  }

  async stop() {
    if (this.mode === 'oray') {
      if (this.oray.summary().status !== 'off' || this.oray.summary().mappingEnabled) await this.oray.stop();
      return this.summary();
    }
    if (this.stopping) return this.summary();
    this.stopping = true;
    this.generation += 1;
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    if (this.pollTimer) clearInterval(this.pollTimer);
    this.pollTimer = null;
    const child = this.process;
    const cleanupAvailable = this.cleanupAvailable;
    this.process = null;
    this.cleanupAvailable = false;
    this.status = 'stopping';
    this.url = null;
    this.error = '';
    if (child && child.exitCode === null && child.signalCode === null) {
      await new Promise((resolve) => {
        let finished = false;
        const finish = () => {
          if (finished) return;
          finished = true;
          clearTimeout(timeout);
          resolve();
        };
        child.once('exit', finish);
        child.once('error', finish);
        const timeout = setTimeout(() => { try { child.kill('SIGKILL'); } catch { /* Already stopped. */ } finish(); }, 5000);
        timeout.unref?.();
        try { child.kill('SIGTERM'); } catch { finish(); }
      });
    }
    this.removeNgrokRuntime();
    if (this.mode === 'tailscale' && (child || cleanupAvailable)) {
      try {
        let observed = await this.tailscaleStatus();
        if (observed.url) {
          await this.runCommand(this.tailscaleCommand, ['funnel', '--https=443', 'off']);
          observed = await this.tailscaleStatus();
        }
        if (observed.portOccupied) throw new Error('Tailscale port still occupied');
      } catch {
        this.status = 'error';
        this.error = '无法确认 Tailscale 公网路由已关闭，请在电脑上运行 tailscale funnel status 检查。';
        this.cleanupAvailable = true;
      }
    }
    if (this.status === 'stopping') this.status = 'off';
    this.stopping = false;
    return this.summary();
  }
}
