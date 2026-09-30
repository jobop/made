import fs from 'node:fs';
import path from 'node:path';
import { createHmac, randomBytes, randomUUID, timingSafeEqual } from 'node:crypto';
import { isIP } from 'node:net';

const ID = /^[0-9a-f]{12}$/;
const CODE = /^[0-9]{6}$/;
const NONCE = /^[0-9a-f]{32}$/;
const PENDING_MS = 120_000;
const APPROVED_RETRY_MS = 60_000;
const PAIRED_DISCOVERY = 'VIBE_BRIDGE_PAIRED_V2';
const MANUAL_VERIFICATION = 'VIBE_BRIDGE_MANUAL_V2';

function writePrivate(file, value) {
  fs.mkdirSync(path.dirname(file), { recursive: true, mode: 0o700 });
  const temporary = `${file}.${process.pid}.${randomBytes(4).toString('hex')}.tmp`;
  fs.writeFileSync(temporary, JSON.stringify(value, null, 2), { mode: 0o600 });
  fs.renameSync(temporary, file);
  fs.chmodSync(file, 0o600);
}

function safeEqual(received, expected) {
  if (typeof received !== 'string' || typeof expected !== 'string') return false;
  const left = Buffer.from(received);
  const right = Buffer.from(expected);
  return left.length === right.length && timingSafeEqual(left, right);
}

export class PairingStore {
  constructor(stateDir) {
    this.file = path.join(stateDir, 'paired-devices.json');
    this.idFile = path.join(stateDir, 'bridge-id.json');
    this.pending = new Map();
    this.paired = new Map();
    this.persistedSeenAt = new Map();
    this.requestTimes = new Map();
    this.verifyTimes = new Map();
    this.activeUntil = 0;
    if (fs.existsSync(this.file)) {
      try {
        const saved = JSON.parse(fs.readFileSync(this.file, 'utf8'));
        for (const item of saved) {
          if (ID.test(item.deviceId) && /^[0-9a-f]{64}$/.test(item.token)) {
            this.paired.set(item.deviceId, item);
            this.persistedSeenAt.set(item.deviceId, Date.parse(item.lastSeen) || 0);
          }
        }
      } catch { /* A damaged state file never authorizes a device. */ }
    }
    try {
      const saved = JSON.parse(fs.readFileSync(this.idFile, 'utf8'));
      if (/^[A-Za-z0-9_-]{1,64}$/.test(saved.bridgeId)) this.bridgeId = saved.bridgeId;
    } catch { /* Create an ID for the first run. */ }
    if (!this.bridgeId) {
      this.bridgeId = randomUUID();
      writePrivate(this.idFile, { bridgeId: this.bridgeId });
    }
  }

  open(seconds = 120) {
    this.activeUntil = Date.now() + Math.min(Math.max(seconds, 1), 300) * 1000;
    return this.summary();
  }

  close() {
    this.activeUntil = 0;
    // Closing the pairing window stops new requests, but an approved device
    // may still be retrying after a lost HTTP response.
    for (const [id, item] of this.pending) {
      if (item.status !== 'approved') this.pending.delete(id);
    }
    return this.summary();
  }

  prune() {
    const now = Date.now();
    for (const [id, item] of this.pending) {
      if (now > item.expiresAt) this.pending.delete(id);
    }
  }

  summary() {
    this.prune();
    return {
      open: Date.now() < this.activeUntil,
      openUntil: this.activeUntil,
      pending: [...this.pending.values()].filter((item) => item.status === 'pending').map(({ deviceId, deviceName, code, nonce, expiresAt, endpoint }) => ({ deviceId, deviceName, code, nonce, expiresAt, endpoint })),
      paired: [...this.paired.values()].map(({ deviceId, deviceName, pairedAt, lastSeen }) => ({
        deviceId, deviceName, pairedAt, lastSeen,
        online: Number.isFinite(Date.parse(lastSeen)) && Date.now() - Date.parse(lastSeen) <= 30_000,
      })),
    };
  }

  request(input, address, endpoint = '') {
    const { deviceId, deviceName, code, nonce } = input || {};
    if (!ID.test(deviceId) || !CODE.test(code) || !NONCE.test(nonce)) throw new Error('配对请求格式无效');
    if (typeof deviceName !== 'string' || deviceName.length > 80) throw new Error('设备名称无效');
    this.prune();
    const prior = this.pending.get(deviceId);
    if (prior && safeEqual(prior.nonce, nonce) && prior.address === address) return { status: prior.status };
    if (Date.now() >= this.activeUntil) throw new Error('电脑配对窗口未开启');
    if (this.pending.size >= 8) throw new Error('待配对设备过多');
    const recent = (this.requestTimes.get(address) || []).filter((time) => Date.now() - time < 60_000);
    if (recent.length >= 10) throw new Error('配对尝试过于频繁');
    recent.push(Date.now());
    this.requestTimes.set(address, recent);
    this.pending.set(deviceId, {
      deviceId, deviceName, code, nonce, address, endpoint,
      status: 'pending', expiresAt: Math.min(Date.now() + PENDING_MS, this.activeUntil),
    });
    return { status: 'pending' };
  }

  status(deviceId, nonce, address) {
    this.prune();
    const item = this.pending.get(deviceId);
    if (!item || !safeEqual(item.nonce, nonce) || item.address !== address) return { status: 'expired' };
    if (item.status === 'approved') {
      return { status: 'approved', token: item.token, bridgeId: this.bridgeId };
    }
    if (item.status === 'denied') {
      this.pending.delete(deviceId);
      return { status: 'denied' };
    }
    return { status: 'pending' };
  }

  decide(deviceId, nonce, action) {
    this.prune();
    const item = this.pending.get(deviceId);
    if (!item || !safeEqual(item.nonce, nonce) || item.status !== 'pending') throw new Error('配对请求已失效');
    if (action === 'reject') {
      item.status = 'denied';
    } else {
      item.status = 'approved';
      item.token = randomBytes(32).toString('hex');
      item.expiresAt = Date.now() + APPROVED_RETRY_MS;
      const now = new Date().toISOString();
      this.paired.set(deviceId, { deviceId, deviceName: item.deviceName, token: item.token, pairedAt: now, lastSeen: now });
      this.persistedSeenAt.set(deviceId, Date.now());
      writePrivate(this.file, [...this.paired.values()]);
      this.activeUntil = 0;
    }
    return this.summary();
  }

  remove(deviceId) {
    if (!this.paired.delete(deviceId)) throw new Error('设备未配对');
    this.persistedSeenAt.delete(deviceId);
    this.pending.delete(deviceId);
    writePrivate(this.file, [...this.paired.values()]);
    return this.summary();
  }

  authenticate(token) {
    if (typeof token !== 'string' || !/^[0-9a-f]{64}$/.test(token)) return false;
    for (const item of this.paired.values()) {
      if (safeEqual(item.token, token)) {
        const now = Date.now();
        item.lastSeen = new Date(now).toISOString();
        if (now - (this.persistedSeenAt.get(item.deviceId) || 0) >= 60_000) {
          writePrivate(this.file, [...this.paired.values()]);
          this.persistedSeenAt.set(item.deviceId, now);
        }
        return true;
      }
    }
    return false;
  }

  signedDiscoveryReply(deviceId, nonce, port, serverIPv4) {
    const paired = this.paired.get(deviceId);
    if (!paired || !NONCE.test(nonce) || !Number.isInteger(port) || port < 1 || port > 65535 ||
        isIP(serverIPv4) !== 4) return null;
    const signed = [PAIRED_DISCOVERY, deviceId, nonce, String(port), this.bridgeId, serverIPv4].join('\n');
    const mac = createHmac('sha256', Buffer.from(paired.token, 'hex')).update(signed, 'ascii').digest('hex');
    return `${PAIRED_DISCOVERY} ${port} ${this.bridgeId} ${serverIPv4} ${mac}`;
  }

  verifyManual(deviceId, nonce, address, authority) {
    if (!ID.test(deviceId || '') || !NONCE.test(nonce || '')) return null;
    if (typeof authority !== 'string' || authority.length > 253 || !authority) return null;
    const now = Date.now();
    const recent = (this.verifyTimes.get(address) || []).filter((time) => now - time < 60_000);
    if (recent.length >= 60) throw new Error('校验请求过于频繁');
    recent.push(now);
    this.verifyTimes.set(address, recent);
    const paired = this.paired.get(deviceId);
    if (!paired) return null;
    const signed = [MANUAL_VERIFICATION, deviceId, nonce, this.bridgeId, authority].join('\n');
    const mac = createHmac('sha256', Buffer.from(paired.token, 'hex')).update(signed, 'ascii').digest('hex');
    return { bridgeId: this.bridgeId, authority, mac };
  }
}
