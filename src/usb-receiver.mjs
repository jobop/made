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
const MAX_REQUEST = 1_100_000;
const MAX_RESPONSE = 262_144;
const REQUEST_TIMEOUT_MS = 30_000;
const VOICE_TIMEOUT_MS = 150_000;
const HELLO_TIMEOUT_MS = 7_000;
const DISCOVERY_INTERVAL_MS = 2_000;
const REJECTED_PORT_COOLDOWN_MS = 60_000;
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

export function parseMacUsbSerialPorts(output) {
  const ports = new Set();
  const blocks = String(output).split(/(?=^[\s|]*\+-o [^\n]*<class IOUSBHostDevice,)/m);
  for (const block of blocks) {
    if (!block.includes('<class IOUSBHostDevice,')) continue;
    const vendor = block.match(/"idVendor"\s*=\s*(\d+)/);
    const product = block.match(/"idProduct"\s*=\s*(\d+)/);
    if (!supportedUsbDevice(Number(vendor?.[1]), Number(product?.[1]))) continue;
    for (const match of block.matchAll(/"IOCalloutDevice"\s*=\s*"(\/dev\/cu\.[A-Za-z0-9._-]+)"/g)) {
      ports.add(match[1]);
    }
  }
  return [...ports];
}

export function linuxUsbSerialPorts({ sysTtyRoot = '/sys/class/tty', devRoot = '/dev' } = {}) {
  const ports = [];
  let names;
  try { names = fs.readdirSync(sysTtyRoot); } catch { return ports; }
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
          if (fs.existsSync(device)) ports.push(device);
          break;
        }
      } catch { /* Walk to the USB device parent. */ }
      directory = path.dirname(directory);
    }
  }
  return ports;
}

export async function discoverUsbSerialPorts(platform = process.platform) {
  if (platform === 'darwin') {
    try {
      const result = await execFileAsync('ioreg', ['-p', 'IOService', '-r', '-c', 'IOUSBHostDevice', '-l', '-t', '-w0'], {
        encoding: 'utf8', timeout: 3000, maxBuffer: 8 * 1024 * 1024,
      });
      return parseMacUsbSerialPorts(result.stdout).filter((port) => fs.existsSync(port));
    } catch { return []; }
  }
  if (platform === 'linux') return linuxUsbSerialPorts();
  return [];
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
    this.helloReceived = false;
    this.peer = null;
    this.connectionId = randomBytes(16).toString('hex');
    this.generation = 0;
    this.current = null;
    this.responding = false;
    this.closed = false;
    this.pendingWork = Promise.resolve();
  }

  feed(chunk) {
    if (this.closed) return;
    let start = 0;
    for (let index = 0; index < chunk.length; index += 1) {
      if (chunk[index] !== 10) continue;
      const segment = chunk.subarray(start, index);
      start = index + 1;
      if (this.discardLine || this.line.length + segment.length + 1 > MAX_LINE) {
        this.line = Buffer.alloc(0);
        this.discardLine = false;
        this.rejectCurrent(400, 'USB 帧过长');
        continue;
      }
      const line = Buffer.concat([this.line, segment]).toString('utf8').replace(/\r$/, '');
      this.line = Buffer.alloc(0);
      this.handleLine(line);
    }
    const remainder = chunk.subarray(start);
    if (this.discardLine) return;
    if (this.line.length + remainder.length + 1 > MAX_LINE) {
      this.line = Buffer.alloc(0);
      this.discardLine = true;
      return;
    }
    this.line = Buffer.concat([this.line, remainder]);
  }

  handleLine(line) {
    let frame;
    try { frame = JSON.parse(line); } catch { this.rejectCurrent(400, 'USB 帧格式无效'); return; }
    if (!frame || typeof frame !== 'object' || Array.isArray(frame)) {
      this.rejectCurrent(400, 'USB 帧格式无效');
      return;
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
      this.peer = peer;
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
    if (frame.type === 'bye' && this.peer.mode === 'direct' &&
        [undefined, 'direct'].includes(frame.mode)) {
      this.cancelCurrent();
      this.helloReceived = false;
      this.generation += 1;
      this.onBye(this.peer);
      return;
    }
    if (frame.type === 'request') {
      if (this.current || this.responding || !validId(frame.id)) {
        if (this.current) this.rejectCurrent(409, 'USB 请求重叠');
        return;
      }
      if (!['GET', 'POST'].includes(frame.method) ||
          typeof frame.path !== 'string' || frame.path.length > 512 ||
          !/^\/(?:device|pair|api)\//.test(frame.path) ||
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
    await this.sendLine(JSON.stringify({ type: 'response', id, status,
      contentType: limitedText(contentType, 128) ? contentType : 'application/octet-stream',
      length: bytes.length }));
    for (let offset = 0; offset < bytes.length; offset += MAX_CHUNK) {
      await this.sendLine(JSON.stringify({ type: 'data', id,
        chunk: bytes.subarray(offset, offset + MAX_CHUNK).toString('base64') }));
    }
    await this.sendLine(JSON.stringify({ type: 'end', id }));
    this.onActivity();
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
    this.closed = true;
    this.cancelCurrent();
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
      const candidates = this.portPath ? [this.portPath] : await discoverUsbSerialPorts();
      if (this.stopped) return;
      if (!candidates.length) {
        if (!this.links.size) {
          this.state.port = this.portPath;
          this.state.error = this.portPath ? 'USB 设备未连接' : '未发现受支持的 USB 设备；可设置 VIBE_USB_PORT';
        }
        return;
      }
      for (const port of candidates) {
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
          // WCH resets the baud rate when its final handle closes. Keep our
          // handle open while stty opens, configures and closes its own handle.
          await configureSerial(port);
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
          const readAvailable = () => {
            if (this.links.get(port) !== link) return;
            // TTY drivers may return only 1 KiB even when more data is ready.
            // Drain a bounded batch, then yield so HTTP and UI stay responsive.
            for (let reads = 0; reads < 64; reads += 1) {
              try {
                const length = fs.readSync(fd, inputBuffer, 0, inputBuffer.length, null);
                if (length > 0) link.protocol.feed(inputBuffer.subarray(0, length));
                else { this.disconnect(link, 'USB 设备已断开'); return; }
              } catch (error) {
                if (error.code === 'EAGAIN' || error.code === 'EWOULDBLOCK') break;
                this.disconnect(link, 'USB 设备已断开');
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
            if (this.link === link && this.state.status !== 'connected') {
              // A round display only says hello while Vibe is open. Keep this
              // nonblocking reader alive so opening Vibe is detected at once,
              // without reopening/resetting the board or delaying other ports.
              this.state.error = 'USB 已连接，等待打开码得应用的 USB 直连';
            }
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
