import fs from 'node:fs';
import path from 'node:path';
import { randomBytes } from 'node:crypto';
import { isIP } from 'node:net';

const ORAY_API = 'https://hsk-api.oray.com';
const API_TIMEOUT_MS = 8000;
const PROBE_TIMEOUT_MS = 5000;

function privateFile(file, value) {
  fs.mkdirSync(path.dirname(file), { recursive: true, mode: 0o700 });
  const temporary = `${file}.${process.pid}.${randomBytes(4).toString('hex')}.tmp`;
  fs.writeFileSync(temporary, value, { mode: 0o600 });
  fs.renameSync(temporary, file);
  fs.chmodSync(file, 0o600);
}

function validateApiKey(value) {
  if (typeof value !== 'string' || value.length < 16 || value.length > 2048 ||
      !/^[A-Za-z0-9._-]+$/.test(value)) {
    throw new Error('花生壳 API Key 格式无效');
  }
  return value;
}

export function normalizeOrayDomain(value) {
  if (typeof value !== 'string' || value.length > 260) throw new Error('请输入花生壳 HTTPS 域名');
  const trimmed = value.trim();
  let parsed;
  try { parsed = new URL(trimmed.includes('://') ? trimmed : `https://${trimmed}`); }
  catch { throw new Error('请输入花生壳 HTTPS 域名'); }
  const hostname = parsed.hostname.toLowerCase();
  const labels = hostname.split('.');
  if (parsed.protocol !== 'https:' || parsed.username || parsed.password || parsed.port ||
      parsed.pathname !== '/' || parsed.search || parsed.hash || hostname.length > 253 ||
      labels.length < 2 || isIP(hostname) || labels.some((label) =>
        !/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(label))) {
    throw new Error('花生壳地址须为 HTTPS 域名，不含端口、路径或参数');
  }
  return hostname;
}

function mapRecord(record) {
  if (!record || typeof record !== 'object') return null;
  const domain = typeof record.domain === 'string' ? record.domain.toLowerCase() : '';
  return {
    domain,
    port: Number(record.port),
    fwtype: Number(record.fwtype),
    servicehost: String(record.servicehost ?? ''),
    serviceport: Number(record.serviceport),
    isforbid: record.isforbid,
    memo: typeof record.memo === 'string' ? record.memo : '',
  };
}

function targetMatches(record, domain, devicePort) {
  return record.domain === domain && record.port === 443 && record.fwtype === 3 &&
    record.servicehost === '127.0.0.1' && record.serviceport === devicePort &&
    typeof record.isforbid === 'boolean';
}

export class OrayManager {
  constructor({ devicePort, stateDir, bridgeId, fetchImpl = fetch }) {
    if (!Number.isInteger(devicePort) || devicePort < 1 || devicePort > 65535) {
      throw new Error('设备接口端口无效');
    }
    this.devicePort = devicePort;
    this.bridgeId = bridgeId;
    this.fetchImpl = fetchImpl;
    this.settingsFile = stateDir ? path.join(stateDir, 'oray-settings.json') : null;
    this.apiKeyFile = stateDir ? path.join(stateDir, 'oray-apikey') : null;
    this.domain = null;
    if (this.settingsFile) {
      try { this.domain = normalizeOrayDomain(JSON.parse(fs.readFileSync(this.settingsFile, 'utf8')).domain); }
      catch { /* Saved settings cannot enable a public mapping on their own. */ }
    }
    this.status = 'off';
    this.url = null;
    this.error = '';
    this.mappingEnabled = false;
    this.busy = false;
  }

  get apiKeyConfigured() {
    try { return Boolean(this.apiKeyFile && validateApiKey(fs.readFileSync(this.apiKeyFile, 'utf8').trim())); }
    catch { return false; }
  }

  summary() {
    return { status: this.status, url: this.url, error: this.error, domain: this.domain,
      mappingEnabled: this.mappingEnabled, apiKeyConfigured: this.apiKeyConfigured };
  }

  configure({ domain, apiKey, clearApiKey = false } = {}) {
    if (this.busy || this.mappingEnabled) throw new Error('请先停用已选花生壳映射，再修改设置');
    if (typeof clearApiKey !== 'boolean' || (clearApiKey && apiKey)) throw new Error('API Key 设置冲突');
    const nextDomain = domain === undefined ? this.domain : normalizeOrayDomain(domain);
    const nextKey = apiKey ? validateApiKey(apiKey) : null;
    if (!this.settingsFile || !this.apiKeyFile) throw new Error('桥接器未配置状态目录');
    if (nextKey) privateFile(this.apiKeyFile, nextKey);
    else if (clearApiKey) fs.rmSync(this.apiKeyFile, { force: true });
    privateFile(this.settingsFile, JSON.stringify({ domain: nextDomain }, null, 2));
    this.domain = nextDomain;
    this.status = 'off';
    this.url = null;
    this.error = '';
    return this.summary();
  }

  async apiRequest(method, route, data = undefined) {
    if (!this.apiKeyConfigured) throw new Error('请先保存花生壳 API Key');
    const secret = validateApiKey(fs.readFileSync(this.apiKeyFile, 'utf8').trim());
    let response;
    try {
      response = await this.fetchImpl(`${ORAY_API}${route}`, {
        method, redirect: 'error', signal: AbortSignal.timeout(API_TIMEOUT_MS),
        headers: { Authorization: `apikey ${secret}`, ...(data === undefined ? {} : { 'content-type': 'application/json' }) },
        ...(data === undefined ? {} : { body: JSON.stringify(data) }),
      });
    } catch { throw new Error('无法连接花生壳开放 API，请检查网络'); }
    if (response.status === 401 || response.status === 403) throw new Error('花生壳 API Key 无效或无权限');
    if (!response.ok) throw new Error(`花生壳开放 API 返回 ${response.status}，请检查账号服务及映射配置`);
    if (response.status === 204) return null;
    try { return await response.json(); }
    catch { throw new Error('花生壳开放 API 返回的数据无效'); }
  }

  async listMappings() {
    const records = await this.apiRequest('GET', '/openapi/v2/mapping/list');
    if (!Array.isArray(records)) throw new Error('花生壳映射列表格式无效');
    return records.map(mapRecord).filter(Boolean);
  }

  async selectedMapping() {
    if (!this.domain) throw new Error('请先选择花生壳 HTTPS 域名');
    const candidates = (await this.listMappings()).filter((record) =>
      record.domain === this.domain && record.port === 443 && record.fwtype === 3);
    if (candidates.length !== 1) {
      throw new Error('未找到唯一的 HTTPS/443 映射，请先在花生壳创建并选择该域名');
    }
    const mapping = candidates[0];
    if (!targetMatches(mapping, this.domain, this.devicePort)) {
      throw new Error(`花生壳映射须指向本机 127.0.0.1:${this.devicePort}，已拒绝操作其他服务`);
    }
    return mapping;
  }

  async setEnabled(enabled) {
    const service = await this.apiRequest('GET', '/openapi/api/forward/service');
    const userId = Number(service?.userid);
    if (!Number.isSafeInteger(userId) || userId < 1) throw new Error('无法读取花生壳账号 ID');
    await this.apiRequest('POST', `/openapi/api/mapping/${userId}/forbid/${enabled ? 'off' : 'on'}`,
      { domain: this.domain, port: 443, fwtype: 3 });
    for (let attempt = 0; attempt < 4; attempt += 1) {
      if (attempt > 0) await new Promise((resolve) => setTimeout(resolve, 350));
      const mapping = await this.selectedMapping();
      this.mappingEnabled = !mapping.isforbid;
      if (this.mappingEnabled === enabled) return;
    }
    throw new Error('花生壳映射状态尚未更新，请稍后刷新状态');
  }

  async publicProbe() {
    if (typeof this.bridgeId !== 'string' || !this.bridgeId) throw new Error('桥接器身份尚未准备好');
    const nonce = randomBytes(16).toString('hex');
    let response;
    try {
      response = await this.fetchImpl(`https://${this.domain}/device/tunnel-probe?nonce=${nonce}`,
        { redirect: 'error', cache: 'no-store', signal: AbortSignal.timeout(PROBE_TIMEOUT_MS) });
    } catch { return false; }
    if (!response.ok) return false;
    try {
      const proof = await response.json();
      return proof?.nonce === nonce && proof.bridgeId === this.bridgeId &&
        proof.authority === this.domain;
    } catch { return false; }
  }

  fail(error) {
    this.status = 'error';
    this.url = null;
    this.error = error instanceof Error ? error.message : String(error);
    return this.summary();
  }

  async refresh() {
    if (this.busy) return this.summary();
    this.busy = true;
    try {
      const mapping = await this.selectedMapping();
      this.mappingEnabled = !mapping.isforbid;
      if (!this.mappingEnabled) {
        this.status = 'off';
        this.url = null;
        this.error = '';
      } else if (await this.publicProbe()) {
        this.status = 'online';
        this.url = `https://${this.domain}`;
        this.error = '';
      } else {
        this.fail(new Error('映射已启用，但无法验证公网地址指向本桥接器；请检查花生壳客户端已登录且映射保留公网 Host'));
      }
      return this.summary();
    } catch (error) { return this.fail(error); }
    finally { this.busy = false; }
  }

  async start() {
    if (this.busy) return this.summary();
    this.busy = true;
    this.status = 'starting';
    this.url = null;
    this.error = '';
    try {
      const mapping = await this.selectedMapping();
      this.mappingEnabled = !mapping.isforbid;
      if (!this.mappingEnabled) await this.setEnabled(true);
      if (!(await this.publicProbe())) {
        return this.fail(new Error('映射已启用，但无法验证公网地址指向本桥接器；请检查花生壳客户端已登录且映射保留公网 Host'));
      }
      this.status = 'online';
      this.url = `https://${this.domain}`;
      return this.summary();
    } catch (error) { return this.fail(error); }
    finally { this.busy = false; }
  }

  async stop() {
    if (this.busy) return this.summary();
    this.busy = true;
    try {
      const mapping = await this.selectedMapping();
      this.mappingEnabled = !mapping.isforbid;
      if (this.mappingEnabled) await this.setEnabled(false);
      this.status = 'off';
      this.url = null;
      this.error = '';
      return this.summary();
    } catch (error) { return this.fail(error); }
    finally { this.busy = false; }
  }
}
