import fs from 'node:fs';
import { translateKnownMessage } from './i18n.mjs';
import path from 'node:path';
import http from 'node:http';
import { execFile } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);
export const USB_RECEIVER_AUTHORITY = '192.168.4.1:8788';
export const USB_DIRECT_AUTHORITY = 'usb.vibe.local:8788';

const MAX_LINE = 2048;
const MAX_CHUNK = 1024;
// 8×1KB 的 base64 行大约 11KB，小于接收器 16KB 接收环。窗口内不等确认，
// 避免 224KB 提示音被逐块往返拖过接收器 30 秒转发上限。
const ACK_WINDOW = 8;
// USB 直连（码得）走 ESP 的 USB Serial/JTAG，固件 RX 环只有 4KB；连发 8 块
// 会灌爆环、丢字节，表现为大响应偶发超时。直连模式收紧到 2 块（约 2.8KB）。
const ACK_WINDOW_DIRECT = 2;
const MAX_REQUEST = 1_100_000;
const MAX_RESPONSE = 262_144;
const REQUEST_TIMEOUT_MS = 30_000;
const VOICE_TIMEOUT_MS = 150_000;
const HELLO_TIMEOUT_MS = 7_000;
const DISCOVERY_INTERVAL_MS = 2_000;
const REJECTED_PORT_COOLDOWN_MS = 60_000;
// 没握手的端口（码得平时只把 USB 当日志口）不能一直占着，否则烧录和串口监视都打不开。
const IDLE_PORT_COOLDOWN_MS = 3_000;
const BASE64 = /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/;
// Probe only the native ESP USB device and the WCH bridge verified on the
// supported C3 receiver. A matching USB ID is a candidate, not a ready peer:
// UsbReceiverProtocol must still validate its hello before forwarding data.
const SUPPORTED_USB_IDS = new Set(['303a:1001', '1a86:55d3']);

function supportedUsbDevice(vendor, product) {
  return Number.isInteger(vendor) && Number.isInteger(product) &&
    SUPPORTED_USB_IDS.has(`${vendor.toString(16)}:${product.toString(16)}`);
}

function validId(id) {
  return Number.isInteger(id) && id >= 0 && id <= 0xffffffff;
}

function isVoiceRequest(method, route) {
  return method === 'POST' && (route === '/device/voice' || route.startsWith('/device/voice?'));
}

function errorBody(message) {
  return Buffer.from(JSON.stringify({ error: message }), 'utf8');
}

function limitedText(value, limit = 128) {
  return typeof value === 'string' && value.length <= limit && !/[\r\n\x00-\x1f\x7f]/.test(value);
}

function usbId(vendor, product) {
  return `${vendor.toString(16)}:${product.toString(16)}`;
}

// 码得的原生 USB Serial/JTAG 不看波特率。对它跑 stty 会改 DTR/RTS，
// ESP32-S3 因此复位，正在用的 Wi-Fi 连接被拔掉。WCH 才是真串口，要设波特率。
export function usbSerialNeedsLineCoding(id) {
  return id !== '303a:1001';
}

export function parseMacUsbSerialDevices(output) {
  const devices = [];
  const seen = new Set();
  const blocks = String(output).split(/(?=^[\s|]*\+-o [^\n]*<class IOUSBHostDevice,)/m);
  for (const block of blocks) {
    if (!block.includes('<class IOUSBHostDevice,')) continue;
    const vendor = block.match(/"idVendor"\s*=\s*(\d+)/);
    const product = block.match(/"idProduct"\s*=\s*(\d+)/);
    const vendorId = Number(vendor?.[1]);
    const productId = Number(product?.[1]);
    if (!supportedUsbDevice(vendorId, productId)) continue;
    const id = usbId(vendorId, productId);
    for (const match of block.matchAll(/"IOCalloutDevice"\s*=\s*"(\/dev\/cu\.[A-Za-z0-9._-]+)"/g)) {
      if (seen.has(match[1])) continue;
      seen.add(match[1]);
      devices.push({ path: match[1], id });
    }
  }
  return devices;
}

export function parseMacUsbSerialPorts(output) {
  return parseMacUsbSerialDevices(output).map((device) => device.path);
}

export function linuxUsbSerialDevices({ sysTtyRoot = '/sys/class/tty', devRoot = '/dev' } = {}) {
  const devices = [];
  let names;
  try { names = fs.readdirSync(sysTtyRoot); } catch { return devices; }
  for (const name of names) {
    if (!/^tty(?:ACM|USB)\d+$/.test(name)) continue;
    let directory;
    try { directory = fs.realpathSync(path.join(sysTtyRoot, name, 'device')); }
    catch { continue; }
    while (directory !== '/') {
      try {
        const vendor = Number.parseInt(fs.readFileSync(path.join(directory, 'idVendor'), 'utf8'), 16);
        const product = Number.parseInt(fs.readFileSync(path.join(directory, 'idProduct'), 'utf8'), 16);
        if (supportedUsbDevice(vendor, product)) {
          const device = path.join(devRoot, name);
          if (fs.existsSync(device)) devices.push({ path: device, id: usbId(vendor, product) });
          break;
        }
      } catch { /* Walk to the USB device parent. */ }
      directory = path.dirname(directory);
    }
  }
  return devices;
}

export function linuxUsbSerialPorts(options) {
  return linuxUsbSerialDevices(options).map((device) => device.path);
}

export async function discoverUsbSerialDevices(platform = process.platform) {
  if (platform === 'darwin') {
    try {
      const result = await execFileAsync('ioreg', ['-p', 'IOService', '-r', '-c', 'IOUSBHostDevice', '-l', '-t', '-w0'], {
        encoding: 'utf8', timeout: 3000, maxBuffer: 8 * 1024 * 1024,
      });
      return parseMacUsbSerialDevices(result.stdout).filter((device) => fs.existsSync(device.path));
    } catch { return []; }
  }
  if (platform === 'linux') return linuxUsbSerialDevices();
  return [];
}

export async function discoverUsbSerialPorts(platform = process.platform) {
  return (await discoverUsbSerialDevices(platform)).map((device) => device.path);
}

async function configureSerial(port, platform = process.platform) {
  if (!['darwin', 'linux'].includes(platform)) return;
  const settings = ['921600', 'raw', '-echo', 'cs8', '-parenb', '-cstopb',
    '-crtscts', 'clocal', '-hupcl'];
  const args = platform === 'darwin'
    ? ['-f', port, ...settings]
    : ['-F', port, ...settings];
  try { await execFileAsync('stty', args, { encoding: 'utf8', timeout: 3000 }); }
  catch { throw new Error('无法配置 USB 串口，请检查端口权限'); }
}

export function forwardUsbRequest(request, { devicePort, authority, forwardSecret, signal }) {
  return new Promise((resolve, reject) => {
    const headers = {
      Host: authority,
      'Content-Length': String(request.body.length),
      'X-Vibe-Usb-Forward': forwardSecret,
    };
    if (request.authorization) headers.Authorization = request.authorization;
    if (request.contentType) headers['Content-Type'] = request.contentType;
    const outgoing = http.request({
      hostname: '127.0.0.1', port: devicePort, method: request.method,
      path: request.path, headers, signal,
    }, (response) => {
      const chunks = [];
      let length = 0;
      response.on('data', (chunk) => {
        length += chunk.length;
        if (length > MAX_RESPONSE) {
          reject(new Error('设备接口响应过大'));
          response.destroy();
          outgoing.destroy();
          return;
        }
        chunks.push(chunk);
      });
      response.on('end', () => {
        if (length > MAX_RESPONSE) return;
        resolve({
          status: response.statusCode || 502,
          contentType: String(response.headers['content-type'] || 'application/octet-stream').slice(0, 128),
          body: Buffer.concat(chunks, length),
        });
      });
      response.on('error', reject);
    });
    outgoing.on('error', reject);
    outgoing.end(request.body);
  });
}

export class UsbReceiverProtocol {
  constructor({ sendLine, forward, onHello = () => {}, onBye = () => {}, onActivity = () => {},
    timeoutMs = REQUEST_TIMEOUT_MS, voiceTimeoutMs = VOICE_TIMEOUT_MS,
    refreshAuthenticatedVoice = () => false, voiceKeepaliveMs = 10_000, locale = () => 'zh-CN' }) {
    this.sendLine = sendLine;
    this.forward = forward;
    this.onHello = onHello;
    this.onBye = onBye;
    this.onActivity = onActivity;
    this.timeoutMs = timeoutMs;
    this.voiceTimeoutMs = voiceTimeoutMs;
    this.refreshAuthenticatedVoice = refreshAuthenticatedVoice;
    this.voiceKeepaliveMs = voiceKeepaliveMs;
    this.locale = locale;
    this.line = Buffer.alloc(0);
    this.discardLine = false;
    this.discardIsFrame = false;
    this.ackWindow = ACK_WINDOW;
    this.helloReceived = false;
    this.peer = null;
    this.connectionId = randomBytes(16).toString('hex');
    this.generation = 0;
    this.current = null;
    this.responding = false;
    this.closed = false;
    this.pendingWork = Promise.resolve();
    // 响应分块的确认计数。接收器仍逐块回 ack；这里最多超前 ACK_WINDOW 块，
    // 既保住 UART 接收环，又不必每一千字节停一次。
    this.ackWaiters = new Map();
    this.ackCredits = new Map();
  }

  feed(chunk) {
    if (this.closed) return;
    if (chunk.includes(0x22) && chunk.includes(0x3a) && chunk.length > 60 && !chunk.slice(0, 30).includes('hello')) {
      console.log('[usb-raw]', chunk.length, JSON.stringify(chunk.subarray(0, 120).toString('utf8')));
    }
    let start = 0;
    for (let index = 0; index < chunk.length; index += 1) {
      if (chunk[index] !== 10) continue;
      const segment = chunk.subarray(start, index);
      start = index + 1;
      if (this.discardLine || this.line.length + segment.length + 1 > MAX_LINE) {
        // 超长行同样要区分日志与坏帧：固件的长日志行不该掐掉在途请求。
        const looksLikeFrame = this.discardLine
          ? this.discardIsFrame
          : (this.line.length ? this.line[0] : segment[0]) === 0x7b;
        if (looksLikeFrame) this.rejectCurrent(400, 'USB 帧过长');
        this.line = Buffer.alloc(0);
        this.discardLine = false;
        this.discardIsFrame = false;
        continue;
      }
      const line = Buffer.concat([this.line, segment]).toString('utf8').replace(/\r$/, '');
      this.line = Buffer.alloc(0);
      this.handleLine(line);
    }
    const remainder = chunk.subarray(start);
    if (this.discardLine) return;
    if (this.line.length + remainder.length + 1 > MAX_LINE) {
      this.discardIsFrame = (this.line.length ? this.line[0] : remainder[0]) === 0x7b;
      this.line = Buffer.alloc(0);
      this.discardLine = true;
      return;
    }
    this.line = Buffer.concat([this.line, remainder]);
  }

  handleLine(line) {
    let frame;
    try { frame = JSON.parse(line); } catch {
      // 固件控制台日志和协议帧共用这条 USB 流（USB Serial/JTAG），日志行随时
      // 会插进来。只有「看着像帧」的坏行才算协议错误；日志必须原样放行，
      // 否则板子一打日志就会掐掉正在传输的响应（素材分片下载失败即此因）。
      if (!line.startsWith('{')) {
        if (/ESP-ROM|rst:0x|boot:0x/.test(line)) {
          console.error(`[trace] usb-boot ${new Date().toISOString()} ${line.slice(0, 180)}`);
        }
        console.error(`[usb-link] log: ${line.slice(0, 240)}`);
        return;
      }
      console.error(`[usb-link] unparseable len=${line.length} tail=${JSON.stringify(line.slice(-40))}`);
      this.rejectCurrent(400, 'USB 帧格式无效');
      return;
    }
    if (!frame || typeof frame !== 'object' || Array.isArray(frame)) {
      this.rejectCurrent(400, 'USB 帧格式无效');
      return;
    }
    if (frame && typeof frame.type === 'string' && frame.type !== 'hello') {
      console.log('[usb-frame]', frame.type, frame.id ?? '', String(frame.path || '').slice(0, 60));
    }
    if (frame.type === 'hello') {
      if (frame.version !== 1 || ![undefined, 'receiver', 'direct'].includes(frame.mode)) return;
      const direct = frame.mode === 'direct';
      if (direct && (!/^[0-9a-f]{12}$/.test(frame.deviceId || '') ||
          !limitedText(frame.deviceName, 80) || !frame.deviceName.trim())) return;
      if (!direct && (!limitedText(frame.ssid, 64) ||
          typeof frame.password !== 'string' || frame.password.length < 8 || frame.password.length > 63 ||
          /[\r\n\x00-\x1f\x7f]/.test(frame.password))) return;
      const peer = { mode: direct ? 'direct' : 'receiver',
        deviceId: direct ? frame.deviceId : '', deviceName: direct ? frame.deviceName : '',
        ssid: direct ? '' : frame.ssid, password: direct ? '' : frame.password };
      if (this.peer && (this.peer.mode !== peer.mode || this.peer.deviceId !== peer.deviceId)) return;
      if (this.onHello(peer) === false) return;
      console.error(`[usb-link] hello: mode=${peer.mode} diag=${frame.diag || '-'} ${new Date().toISOString()}`);
      this.peer = peer;
      this.ackWindow = peer.mode === 'direct' ? ACK_WINDOW_DIRECT : ACK_WINDOW;
      this.helloReceived = true;
      this.onActivity();
      // The receiver waits for this acknowledgement before forwarding HTTP.
      // A USB cable being present does not mean the desktop process is ready.
      const generation = this.generation;
      this.pendingWork = this.pendingWork.then(async () => {
        if (!this.closed && this.helloReceived && this.generation === generation) {
          await this.sendLine(JSON.stringify({ type: 'ready', version: 1, connectionId: this.connectionId }));
        }
      }).catch(() => this.dispose());
      return;
    }
    if (!this.helloReceived) return;
    if (frame.type === 'ack' && validId(frame.id)) {
      if (this.ackCredits.has(frame.id)) {
        this.ackCredits.set(frame.id, this.ackCredits.get(frame.id) + 1);
      }
      const waiter = this.ackWaiters.get(frame.id);
      if (waiter) waiter();
      return;
    }
    if (frame.type === 'bye' && this.peer.mode === 'direct' &&
        [undefined, 'direct'].includes(frame.mode)) {
      this.cancelCurrent();
      this.helloReceived = false;
      this.generation += 1;
      this.onBye(this.peer);
      return;
    }
    if (frame.type === 'request') {
      console.log('[usb-req]', frame.method, frame.path, 'id=' + frame.id);
      if (this.current || this.responding || !validId(frame.id)) {
        if (this.current) this.rejectCurrent(409, 'USB 请求重叠');
        return;
      }
      if (!['GET', 'POST'].includes(frame.method) ||
          typeof frame.path !== 'string' || frame.path.length > 512 ||
          /[\r\n\x00-\x1f\x7f#]/.test(frame.path) ||
          !limitedText(frame.authorization || '', 128) ||
          !limitedText(frame.contentType || '', 128) ||
          !Number.isInteger(frame.length) || frame.length < 0 || frame.length > MAX_REQUEST) {
        this.queueResponse(frame.id, 400, this.errorBody('USB 请求格式无效'));
        return;
      }
      const current = {
        id: frame.id, method: frame.method, path: frame.path, mode: this.peer.mode,
        authorization: frame.authorization || '', contentType: frame.contentType || '',
        length: frame.length, chunks: [], received: 0,
        controller: new AbortController(),
      };
      current.timeoutMs = isVoiceRequest(frame.method, frame.path) ? this.voiceTimeoutMs : this.timeoutMs;
      current.timer = setTimeout(() => this.timeout(current), current.timeoutMs);
      current.timer.unref?.();
      this.current = current;
      if (isVoiceRequest(frame.method, frame.path) && this.refreshVoice(current)) {
        current.keepaliveTimer = setInterval(() => {
          if (this.current !== current || this.closed || current.controller.signal.aborted ||
              !this.refreshVoice(current)) {
            clearInterval(current.keepaliveTimer);
            current.keepaliveTimer = null;
          }
        }, this.voiceKeepaliveMs);
        current.keepaliveTimer.unref?.();
      }
      this.onActivity();
      return;
    }
    const current = this.current;
    if (!current || frame.id !== current.id) return;
    if (frame.type === 'cancel') {
      // Leaving Vibe disconnects the display from the receiver AP. Stop both
      // the upstream operation and its temporary device-online refresh.
      this.cancelCurrent();
      this.onActivity();
      return;
    }
    if (frame.type === 'data') {
      if (typeof frame.chunk !== 'string' || frame.chunk.length > 1368 ||
          !BASE64.test(frame.chunk)) {
        this.rejectCurrent(400, 'USB 数据帧无效');
        return;
      }
      const decoded = Buffer.from(frame.chunk, 'base64');
      if (decoded.length === 0 || decoded.length > MAX_CHUNK ||
          decoded.toString('base64') !== frame.chunk ||
          current.received + decoded.length > current.length) {
        this.rejectCurrent(400, 'USB 数据长度无效');
        return;
      }
      current.chunks.push(decoded);
      current.received += decoded.length;
      this.onActivity();
      return;
    }
    if (frame.type === 'end') {
      console.log('[usb-end] id=' + current.id + ' received=' + current.received + ' path=' + current.path.slice(0, 40));
      if (current.received !== current.length) {
        this.rejectCurrent(400, 'USB 请求长度不符');
        return;
      }
      this.responding = true;
      const request = { ...current, body: Buffer.concat(current.chunks, current.length) };
      this.pendingWork = this.pendingWork.then(async () => {
        try {
          if (this.closed || this.current !== current || current.controller.signal.aborted) return;
          const response = await this.forward(request, current.controller.signal);
          console.log('[usb-resp]', response.status, request.method, request.path.slice(0, 40),
            'body=' + (response.body ? response.body.length : -1));
          if (this.closed || this.current !== current) return;
          clearTimeout(current.timer);
          await this.writeResponse(current.id, response.status, response.contentType, response.body);
        } catch (error) {
          if (this.closed || this.current !== current) return;
          const status = current.controller.signal.aborted ? 504 : 502;
          await this.writeResponse(current.id, status, 'application/json; charset=utf-8',
            this.errorBody(status === 504 ? 'USB 请求超时' : '电脑设备接口暂不可用'));
        } finally {
          this.finish(current);
        }
      }).catch(() => this.finish(current));
      return;
    }
    this.rejectCurrent(400, 'USB 帧类型无效');
  }

  timeout(current) {
    if (this.current !== current || this.closed) return;
    current.controller.abort();
    this.current = null;
    this.responding = true;
    clearTimeout(current.timer);
    clearInterval(current.keepaliveTimer);
    this.queueResponse(current.id, 504, this.errorBody('USB 请求超时'));
  }

  rejectCurrent(status, message) {
    const current = this.current;
    if (!current) return;
    current.controller.abort();
    this.current = null;
    this.responding = true;
    clearTimeout(current.timer);
    clearInterval(current.keepaliveTimer);
    this.queueResponse(current.id, status, this.errorBody(message));
  }

  queueResponse(id, status, body) {
    this.responding = true;
    this.pendingWork = this.pendingWork.then(() =>
      this.writeResponse(id, status, 'application/json; charset=utf-8', body)
    ).catch(() => {}).finally(() => { if (!this.current) this.responding = false; });
  }

  async writeResponse(id, status, contentType, body) {
    if (this.closed) return;
    const bytes = Buffer.isBuffer(body) ? body : Buffer.from(body || '');
    if (bytes.length > MAX_RESPONSE) {
      await this.writeResponse(id, 502, 'application/json; charset=utf-8', this.errorBody('设备接口响应过大'));
      return;
    }
    this.ackCredits.set(id, 0);
    try {
      await this.sendLine(JSON.stringify({ type: 'response', id, status,
        contentType: limitedText(contentType, 128) ? contentType : 'application/octet-stream',
        length: bytes.length }));
      let sentChunks = 0;
      for (let offset = 0; offset < bytes.length; offset += MAX_CHUNK) {
        while (sentChunks - (this.ackCredits.get(id) || 0) >= this.ackWindow) {
          await this.waitForCredits(id, sentChunks - this.ackWindow + 1);
        }
        const piece = bytes.subarray(offset, offset + MAX_CHUNK);
        // 逐帧指纹：这是【所有】USB 响应分片（sessions/commands 轮询也会打），
        // 不是素材专属日志。与固件 [sprite-rx] 对齐比对仅在素材下载窗口内有效，
        // 能定位到具体哪一帧被改——别的请求别拿这里的指纹对账（2026-10-14 排障
        // 曾因把 15B/42B 轮询响应当素材分片绕了远路）。
        if (process.env.VIBE_UI_DIAGNOSTICS === '1') {
          let fnv = 2166136261 >>> 0;
          for (const byte of piece) fnv = Math.imul(fnv ^ byte, 16777619) >>> 0;
          console.log(`[usb-tx] id=${id} off=${offset} len=${piece.length} fnv1a=${fnv.toString(16).padStart(8, '0')}`);
        }
        await this.sendLine(JSON.stringify({ type: 'data', id,
          chunk: piece.toString('base64') }));
        sentChunks += 1;
      }
      if (sentChunks > (this.ackCredits.get(id) || 0)) await this.waitForCredits(id, sentChunks);
      await this.sendLine(JSON.stringify({ type: 'end', id }));
      console.error(`[usb-link] response done id=${id} bytes=${bytes.length} ${new Date().toISOString()}`);
      this.onActivity();
    } finally {
      this.ackCredits.delete(id);
      this.ackWaiters.delete(id);
    }
  }

  waitForCredits(id, target) {
    if ((this.ackCredits.get(id) || 0) >= target) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        console.error(`[usb-link] ack TIMEOUT id=${id} ${new Date().toISOString()}`);
        this.ackWaiters.delete(id);
        reject(new Error('USB ACK 超时'));
      }, 15_000);
      timer.unref?.();
      const check = () => {
        if ((this.ackCredits.get(id) || 0) < target) return;
        clearTimeout(timer);
        this.ackWaiters.delete(id);
        resolve();
      };
      this.ackWaiters.set(id, check);
      check();
    });
  }

  finish(current) {
    clearTimeout(current.timer);
    clearInterval(current.keepaliveTimer);
    if (this.current === current) {
      this.current = null;
      this.responding = false;
    }
  }

  dispose() {
    console.error(`[usb-link] dispose ${new Date().toISOString()}`);
    this.closed = true;
    this.cancelCurrent();
    this.ackWaiters.clear();
    this.ackCredits.clear();
    this.line = Buffer.alloc(0);
  }

  cancelCurrent() {
    const current = this.current;
    this.current = null;
    this.responding = false;
    current?.controller.abort();
    clearTimeout(current?.timer);
    clearInterval(current?.keepaliveTimer);
  }

  errorBody(message) { return errorBody(translateKnownMessage(message, this.locale())); }

  refreshVoice(current) {
    try { return this.refreshAuthenticatedVoice(current.authorization) === true; }
    catch { return false; }
  }
}

export class UsbReceiver {
  constructor({ devicePort, portPath = '', authority = USB_RECEIVER_AUTHORITY, forwardSecret = '',
    refreshAuthenticatedVoice = () => false, locale = () => 'zh-CN' }) {
    this.devicePort = devicePort;
    this.portPath = portPath;
    this.locale = locale;
    this.authority = authority;
    this.forwardSecret = forwardSecret;
    this.refreshAuthenticatedVoice = refreshAuthenticatedVoice;
    this.link = null;
    this.links = new Map();
    this.polling = false;
    this.stopped = true;
    this.cooldown = new Map();
    this.timer = null;
    this.state = {
      status: process.platform === 'darwin' || process.platform === 'linux' || portPath ? 'disconnected' : 'disabled',
      port: portPath, mode: '', active: false, deviceId: '', deviceName: '', ssid: '', password: '', lastSeen: '',
      error: process.platform === 'darwin' || process.platform === 'linux' || portPath ? '' : '此系统需手动配置 VIBE_USB_PORT',
    };
  }

  summary() { return { ...this.state, error: translateKnownMessage(this.state.error, this.locale()) }; }

  start() {
    if (!this.stopped) return;
    this.stopped = false;
    this.timer = setInterval(() => { void this.poll(); }, DISCOVERY_INTERVAL_MS);
    this.timer.unref?.();
    void this.poll();
  }

  async poll() {
    if (this.link && this.state.status === 'connected' &&
        !this.link.protocol?.current && !this.link.protocol?.responding &&
        Date.now() - Date.parse(this.state.lastSeen) > 20_000) {
      this.disconnect(this.link, 'USB 握手心跳超时');
    }
    if (this.polling || this.stopped || this.state.status === 'disabled') return;
    this.polling = true;
    try {
      const devices = this.portPath ? [{ path: this.portPath, id: '' }] : await discoverUsbSerialDevices();
      if (this.stopped) return;
      if (!devices.length) {
        if (!this.links.size) {
          this.state.port = this.portPath;
          this.state.error = this.portPath ? 'USB 设备未连接' : '未发现受支持的 USB 设备；可设置 VIBE_USB_PORT';
        }
        return;
      }
      for (const device of devices) {
        const port = device.path;
        if (this.stopped) break;
        if (this.links.has(port)) continue;
        if (Date.now() < (this.cooldown.get(port) || 0)) continue;
        let unregisteredFd = null;
        let openedLink = null;
        try {
          // A blocking read on a serial FD can make closeSync wait forever when
          // the attached ESP32 is the round display rather than a receiver.
          const fd = fs.openSync(port, fs.constants.O_RDWR | fs.constants.O_NONBLOCK |
            (fs.constants.O_NOCTTY || 0));
          unregisteredFd = fd;
          const lineCoding = usbSerialNeedsLineCoding(device.id);
          console.error(`[trace] usb-open port=${port} id=${device.id || '-'} stty=${lineCoding} ${new Date().toISOString()}`);
          // WCH resets the baud rate when its final handle closes. Keep our
          // handle open while stty opens, configures and closes its own handle.
          // 码得自己的口跳过这一步，避免插入 USB 时把 Wi-Fi 连接复位掉。
          if (lineCoding) await configureSerial(port);
          if (this.stopped) break;
          const link = { fd, port, helloTimer: null, inputTimer: null,
            protocol: null, writeQueue: Promise.resolve() };
          this.links.set(port, link);
          openedLink = link;
          unregisteredFd = null;
          if (!this.link) {
            this.link = link;
            this.state.port = port;
            this.state.status = 'waiting';
            this.state.error = '';
          }
          const sendLine = (line) => {
            const bytes = Buffer.from(`${line}\n`, 'utf8');
            if (bytes.length > MAX_LINE) return Promise.reject(new Error('USB 响应帧过长'));
            link.writeQueue = link.writeQueue.then(() => new Promise((resolve, reject) => {
              const deadline = Date.now() + 5_000;
              const writeRemaining = (offset) => {
                if (this.links.get(port) !== link) { reject(new Error('USB 设备已断开')); return; }
                fs.write(link.fd, bytes, offset, bytes.length - offset, null, (error, written) => {
                  if (error?.code === 'EAGAIN' || error?.code === 'EWOULDBLOCK' || (!error && !written)) {
                    if (Date.now() >= deadline) reject(new Error('USB 写入超时'));
                    else setTimeout(() => writeRemaining(offset), 10);
                  } else if (error) reject(error);
                  else if (offset + written === bytes.length) resolve();
                  else writeRemaining(offset + written);
                });
              };
              writeRemaining(0);
            }));
            link.writeQueue.catch(() => this.disconnect(link, 'USB 设备已断开'));
            return link.writeQueue;
          };
          link.protocol = new UsbReceiverProtocol({
            sendLine, locale: this.locale,
            forward: (request, signal) => forwardUsbRequest(request, {
              devicePort: this.devicePort,
              authority: request.mode === 'direct' ? USB_DIRECT_AUTHORITY : this.authority,
              forwardSecret: this.forwardSecret, signal,
            }),
            onHello: (peer) => {
              if (this.links.get(port) !== link || (this.state.active && this.link !== link)) return false;
              this.link = link;
              clearTimeout(link.helloTimer);
              this.state = { status: 'connected', active: true, port, ...peer,
                lastSeen: new Date().toISOString(), error: '' };
            },
            onBye: () => {
              if (this.link !== link) return;
              this.state = { ...this.state, status: 'waiting', active: false, error: '' };
            },
            onActivity: () => {
              if (this.link === link && this.state.status === 'connected') {
                this.state.lastSeen = new Date().toISOString();
              }
            },
            refreshAuthenticatedVoice: this.refreshAuthenticatedVoice,
          });
          const inputBuffer = Buffer.allocUnsafe(4096);
          // macOS 的 CDC 驱动偶发返回 0 字节读（并非拔线）。连续空读超过
          // ~1s 才判死，否则一次抖动就把链路拆掉重连（实机表现为「时不时断开」）。
          const ZERO_READ_LIMIT = 50;
          link.zeroReads = 0;
          const readAvailable = () => {
            if (this.links.get(port) !== link) return;
            // TTY drivers may return only 1 KiB even when more data is ready.
            // Drain a bounded batch, then yield so HTTP and UI stay responsive.
            for (let reads = 0; reads < 64; reads += 1) {
              try {
                const length = fs.readSync(fd, inputBuffer, 0, inputBuffer.length, null);
                if (length > 0) {
                  link.zeroReads = 0;
                  link.protocol.feed(inputBuffer.subarray(0, length));
                } else if (this.links.get(port) === link && ++link.zeroReads >= ZERO_READ_LIMIT) {
                  this.disconnect(link, 'USB 设备已断开');
                  return;
                } else break;
              } catch (error) {
                if (error.code === 'EAGAIN' || error.code === 'EWOULDBLOCK') break;
                this.disconnect(link, `USB 读取错误 ${error.code || error.message}`);
                return;
              }
            }
            if (this.links.get(port) !== link) return;
            link.inputTimer = setTimeout(readAvailable,
              link.protocol.current || link.protocol.responding ? 1 : 20);
            link.inputTimer.unref?.();
          };
          link.inputTimer = setTimeout(readAvailable, 20);
          link.inputTimer.unref?.();
          link.helloTimer = setTimeout(() => {
            if (this.links.get(port) !== link || this.state.status === 'connected') return;
            // 没握手就先放开串口，烧录可以插空占用。冷却只有几秒，板子每 3 秒
            // 发一次 hello，下一轮打开就能接上，不用对准打开的时刻。
            this.cooldown.set(port, Date.now() + IDLE_PORT_COOLDOWN_MS);
            this.disconnect(link, '未握手，已暂时释放串口，马上会再试');
          }, HELLO_TIMEOUT_MS);
          link.helloTimer.unref?.();
        } catch {
          if (openedLink) this.disconnect(openedLink);
          if (!this.stopped) {
            if (!this.state.active) this.state.error = '无法打开 USB 串口，请检查端口权限或占用情况';
            this.cooldown.set(port, Date.now() + REJECTED_PORT_COOLDOWN_MS);
          }
        } finally {
          if (unregisteredFd !== null) {
            await new Promise((resolve) => fs.close(unregisteredFd, () => resolve()));
          }
        }
      }
    } finally { this.polling = false; }
  }

  disconnect(link, error = '') {
    if (this.links.get(link.port) !== link) return;
    // 断开原因必须留痕：连接状态只存最后一条 error，没有这行日志就查不到
    // 「时不时断开」到底是空读判死、写超时还是心跳超时。
    console.error(`[usb-link] disconnect port=${link.port} reason=${error || '未指定'} lastSeen=${this.state.lastSeen || 'never'}`);
    this.links.delete(link.port);
    clearTimeout(link.helloTimer);
    link.protocol?.dispose();
    clearTimeout(link.inputTimer);
    fs.close(link.fd, () => {});
    if (this.link === link) {
      this.link = this.links.values().next().value || null;
      this.state = { status: this.link ? 'waiting' : 'disconnected', active: false,
        port: this.link?.port || this.portPath || '', mode: '', deviceId: '', deviceName: '', ssid: '', password: '',
        lastSeen: this.state.lastSeen, error };
    }
  }

  async stop() {
    this.stopped = true;
    clearInterval(this.timer);
    for (const link of this.links.values()) this.disconnect(link);
  }
}
