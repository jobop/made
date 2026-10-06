import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import dgram from 'node:dgram';
import os from 'node:os';
import { isIP } from 'node:net';
import { randomUUID, timingSafeEqual } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { loadConfig, ROOT } from './config.mjs';
import { JobStore } from './jobs.mjs';
import { VIBE_SESSION_ID } from './sessions.mjs';
import { createRunner, providerStates, isBuiltinAgent } from './providers.mjs';
import { loadLanguageSettings, saveLanguageSettings, validateLocale } from './language-settings.mjs';
import { translateKnownMessage, localizeStatus } from './i18n.mjs';
import { transcribe as transcribeVoice, transcriptionState, validateVoiceWav } from './transcribe.mjs';
import { PairingStore } from './pairing.mjs';
import { validateOpenAIKey } from './openai-key.mjs';
import { validateTranscribeModel, validateTranscribeUrl } from './voice-settings.mjs';
import { saveModelSettings, validateModelSettings } from './model-settings.mjs';
import { TunnelManager } from './tunnel.mjs';
import { UsbReceiver, USB_DIRECT_AUTHORITY, USB_RECEIVER_AUTHORITY } from './usb-receiver.mjs';

import { createPluginRegistry, loadConfiguredPlugins } from './plugins/registry.mjs';
import { listInstalled, removePlugin, removeTheme, savePluginSettings, setPluginEnabled, themeExists } from './installed.mjs';
import { createBoardLink } from './board-link.mjs';
import { initializeSpeechSettings, saveSpeechSettings } from './speech-settings.mjs';
import { builtinSpeechRecognizers } from './speech/index.mjs';
const PAIRED_DISCOVERY_REQUEST = /^VIBE_DISCOVER_PAIRED_V2 ([0-9a-f]{12}) ([0-9a-f]{32})$/;

function discoveryName() {
  // This is a display label, never a hostname used for requests. Remove
  // controls (including bidi overrides) and truncate at UTF-8 boundaries.
  const hostname = os.hostname().replace(/[\p{Cc}\p{Cf}\p{Cs}\p{Zl}\p{Zp}]/gu, ' ')
    .replace(/\s+/gu, ' ').trim();
  let name = '';
  let bytes = 0;
  for (const character of hostname) {
    const size = Buffer.byteLength(character);
    if (bytes + size > 64) break;
    name += character;
    bytes += size;
  }
  return name.trim() || 'Vibe Bridge';
}

function normalizedAuthority(host) {
  if (typeof host !== 'string' || host.length > 253 || /[\s/@?#]/.test(host)) return null;
  try {
    const url = new URL(`http://${host}`);
    if (!url.hostname || url.pathname !== '/' || url.username || url.password) return null;
    const hostname = url.hostname.toLowerCase();
    const port = url.port && !['80', '443'].includes(url.port) ? `:${url.port}` : '';
    return `${hostname}${port}`;
  } catch { return null; }
}

function localAuthorities(port) {
  const addresses = new Set(['127.0.0.1', 'localhost']);
  for (const interfaces of Object.values(os.networkInterfaces())) {
    for (const item of interfaces || []) if (item.family === 'IPv4' && isIP(item.address) === 4) addresses.add(item.address);
  }
  const suffix = [80, 443].includes(port) ? '' : `:${port}`;
  return new Set([...addresses].map((address) => `${address}${suffix}`));
}

function outboundIPv4(remote) {
  return new Promise((resolve, reject) => {
    const probe = dgram.createSocket('udp4');
    let finished = false;
    const finish = (error, address) => {
      if (finished) return;
      finished = true;
      probe.close();
      if (error) reject(error);
      else resolve(address);
    };
    probe.once('error', (error) => finish(error));
    probe.connect(remote.port, remote.address, () => {
      try { finish(null, probe.address().address); }
      catch (error) { finish(error); }
    });
  });
}

function json(res, status, value) {
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
  });
  res.end(JSON.stringify(value));
}

function equalToken(received, expected) {
  if (!received || !expected) return false;
  const left = Buffer.from(received);
  const right = Buffer.from(expected);
  return left.length === right.length && timingSafeEqual(left, right);
}

function deviceText(value) {
  return String(value || '')
    .replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '')
    .replace(/\r\n?/g, '\n')
    .replace(/\t/g, '  ')
    .replace(/[\u0000-\u0009\u000b-\u001f\u007f]/g, ' ')
    // The board's compact Puhui font has no emoji glyphs. Keep the desktop
    // result intact, but omit emoji from the device copy instead of tofu boxes.
    .replace(/[\p{Extended_Pictographic}\p{Emoji_Modifier}\uFE0E\uFE0F\u200D]/gu, '');
}

function devicePreview(value, maxBytes) {
  let result = '';
  let bytes = 0;
  for (const character of deviceText(value)) {
    const size = Buffer.byteLength(character);
    if (bytes + size > maxBytes) break;
    result += character;
    bytes += size;
  }
  return result;
}

function deviceResultPreview(value, maxBytes) {
  const full = deviceText(value);
  if (Buffer.byteLength(full) <= maxBytes) return { text: full, truncated: false };

  // Keep the beginning for context and the end for follow-up messages such as
  // WorkBuddy's later replies. A prefix-only preview would never show updates
  // once the initial reply reached this transport limit.
  const separator = '\n…\n';
  const prefix = devicePreview(full, Math.floor(maxBytes / 3));
  const remaining = maxBytes - Buffer.byteLength(prefix) - Buffer.byteLength(separator);
  const characters = Array.from(full);
  const suffix = [];
  let bytes = 0;
  for (let index = characters.length - 1; index >= 0; index -= 1) {
    const size = Buffer.byteLength(characters[index]);
    if (bytes + size > remaining) break;
    suffix.push(characters[index]);
    bytes += size;
  }
  return { text: prefix + separator + suffix.reverse().join(''), truncated: true };
}

function publicModels(config) {
  return Object.fromEntries(config.pluginsRuntime.agents().map(plugin => [plugin.id,
    plugin.capabilities.model ? (config.models?.[plugin.id] ?? (plugin.id === 'codex' ? config.codexModel : undefined) ?? plugin.model?.default ?? '') : null,
  ]));
}

function listenServer(server, port, host) {
  return new Promise((resolve, reject) => {
    const onError = (error) => {
      server.off('listening', onListening);
      reject(error);
    };
    const onListening = () => {
      server.off('error', onError);
      resolve();
    };
    server.once('error', onError);
    server.once('listening', onListening);
    server.listen(port, host);
  });
}

async function body(req) {
  let data = '';
  for await (const chunk of req) {
    data += chunk.toString('utf8');
    if (data.length > 10000) throw new Error('请求过大');
  }
  return data ? JSON.parse(data) : {};
}

async function binaryBody(req, maxBytes) {
  const chunks = [];
  let size = 0;
  for await (const chunk of req) {
    size += chunk.length;
    if (size > maxBytes) throw new Error('录音超过 30 秒上限');
    chunks.push(chunk);
  }
  return Buffer.concat(chunks, size);
}

export function createApp(config, { run, transcribe, tunnel: providedTunnel, enableUsb = false } = {}) {
  config.locale = loadLanguageSettings(config.stateDir, config.locale ?? 'zh-CN');
  const message = (value, locale = config.locale) => translateKnownMessage(value, locale);
  const send = (res, status, value, translate = true, locale = config.locale) => json(res, status,
    translate && value && Object.keys(value).length === 1 && typeof value.error === 'string'
      ? { error: message(value.error, locale) } : value);
  const publicSession = (session, locale = config.locale) => ({ ...session,
    ...(session.autoTitlePending ? { title: message(session.title, locale) } : {}) });
  const publicJob = (job, locale = config.locale) => {
    const builtin = isBuiltinAgent(config.pluginsRuntime.agent(job.provider));
    return { ...job, ...(builtin ? { progress: message(job.progress, locale), error: message(job.error, locale),
      ...(job.handoffReadError ? { handoffReadError: message(job.handoffReadError, locale) } : {}) } : {}) };
  };
  const publicVoice = () => {
    const voice = transcriptionState(config);
    const builtin = (id) => builtinSpeechRecognizers.some(item => item.id === id &&
      item.transcribe === config.pluginsRuntime.speechRecognizer(id)?.transcribe);
    return { ...voice, reason: builtin(voice.pluginId) || !config.pluginsRuntime.speechRecognizer(voice.pluginId)
      ? message(voice.reason) : voice.reason,
      plugins: voice.plugins.map(plugin => builtin(plugin.id) ? ({
        ...plugin, label: message(plugin.label), reason: message(plugin.reason),
        fields: plugin.fields.map(field => ({ ...field, label: message(field.label) })),
        ...(plugin.presets ? { presets: plugin.presets.map(preset => ({ ...preset, label: message(preset.label) })) } : {}),
      }) : plugin) };
  };
  const publicTunnel = (value = tunnel.summary()) => localizeStatus(value, config.locale);
  if (!config.pluginsRuntime && (config.plugins?.length || config.disabledPlugins?.length)) throw new Error('请先 await loadConfiguredPlugins(config) 并赋值 config.pluginsRuntime');
  config.pluginsRuntime ||= createPluginRegistry();
  if (!config.pluginsRuntime.hasAgent(config.defaultProvider)) throw new Error('默认编程助手未安装或已禁用');
  if (!config.speech) initializeSpeechSettings(config);
  config.models = { ...Object.fromEntries(config.pluginsRuntime.agents().filter(p => p.capabilities.model).map(p => [p.id, p.model?.default || ''])), ...(config.codexModel ? {codex: config.codexModel} : {}), ...config.models };
  validateModelSettings(Object.fromEntries(Object.entries(publicModels(config)).filter(([, value]) => value !== null)), config.pluginsRuntime);
  const sessionToken = randomUUID();
  const usbForwardSecret = randomUUID();
  const pairing = new PairingStore(config.stateDir);
  const boardLink = createBoardLink();
  const tunnel = providedTunnel || new TunnelManager({
    devicePort: config.devicePort, stateDir: config.stateDir, bridgeId: pairing.bridgeId,
  });
  const receiver = new UsbReceiver({
    devicePort: config.devicePort,
    locale: () => config.locale,
    portPath: config.usbPort || '',
    authority: USB_RECEIVER_AUTHORITY,
    forwardSecret: usbForwardSecret,
    refreshAuthenticatedVoice: (authorization) => {
      const token = /^Bearer\s+([0-9a-f]{64})$/i.exec(authorization)?.[1];
      return token ? pairing.authenticate(token) : false;
    },
  });
  let publicRefresh = null;
  const discovery = dgram.createSocket('udp4');
  const discoveryPort = config.discoveryPort ?? 8789;
  discovery.on('message', (message, remote) => {
    const request = message.toString('utf8');
    if (request === 'VIBE_DISCOVER_V1' || request === 'VIBE_DISCOVER_V3') {
      if (pairing.paired.size === 0 && Date.now() >= pairing.activeUntil) pairing.open();
      // Discovery only lists candidates. The selected display must explicitly
      // POST /pair/request before a code appears in the desktop dashboard.
      const response = request === 'VIBE_DISCOVER_V3'
        ? Buffer.from(JSON.stringify({
          type: 'VIBE_BRIDGE_V3', version: 3, port: config.devicePort,
          bridgeId: pairing.bridgeId, name: discoveryName(),
          pairingOpen: Date.now() < pairing.activeUntil,
        }), 'utf8')
        : Buffer.from(`VIBE_BRIDGE_V1 ${config.devicePort} ${pairing.bridgeId}`);
      discovery.send(response, remote.port, remote.address);
      return;
    }
    const paired = request.match(PAIRED_DISCOVERY_REQUEST);
    if (!paired || !pairing.paired.has(paired[1])) return;
    void outboundIPv4(remote).then((serverIPv4) => {
      const response = pairing.signedDiscoveryReply(paired[1], paired[2], config.devicePort, serverIPv4);
      if (response) discovery.send(Buffer.from(response, 'ascii'), remote.port, remote.address);
    }).catch(() => { /* An unreachable discovery requester needs no reply. */ });
  });
  let voiceBusy = false;
  const connection = {
    deviceApi: 'enabled',
  };
  const store = new JobStore({
    file: path.join(config.stateDir, 'jobs.json'),
    projects: config.projects,
    workspaceRoot: config.workspaceRoot,
    defaultProject: config.defaultProject,
    defaultProvider: config.defaultProvider,
    pluginsRuntime: config.pluginsRuntime,
    run: run || createRunner(config),
    // Keep stored built-in failures canonical; each client localizes at its output boundary.
    providerAvailable: (id) => ({ ...providerStates(config, 'zh-CN').find((item) => item.id === id),
      preserveMessage: !isBuiltinAgent(config.pluginsRuntime.agent(id)) }),
  });
  const desktop = http.createServer(async (req, res) => {
    try {
      if (![`127.0.0.1:${config.port}`, `localhost:${config.port}`].includes(req.headers.host)) {
        send(res, 403, { error: '只允许从本机访问电脑面板' });
        return;
      }
      const url = new URL(req.url, 'http://localhost');
      const staticFiles = {
        '/': ['index.html', 'text/html; charset=utf-8'],
        '/assistants.html': ['assistants.html', 'text/html; charset=utf-8'],
        '/tasks.html': ['tasks.html', 'text/html; charset=utf-8'],
        '/plugins.html': ['plugins.html', 'text/html; charset=utf-8'],
        '/plugins.js': ['plugins.js', 'text/javascript; charset=utf-8'],
        '/styles.css': ['styles.css', 'text/css; charset=utf-8'],
        '/app.js': ['app.js', 'text/javascript; charset=utf-8'],
        '/provider-icon.js': ['provider-icon.js', 'text/javascript; charset=utf-8'],
        '/i18n.js': ['i18n.js', 'text/javascript; charset=utf-8'],
        '/i18n-static.js': ['i18n-static.js', 'text/javascript; charset=utf-8'],
        '/i18n-messages.js': ['i18n-messages.js', 'text/javascript; charset=utf-8'],
        '/made-icon.png': ['made-icon.png', 'image/png'],
      };
      if (req.method === 'GET' && Object.hasOwn(staticFiles, url.pathname)) {
        const [file, contentType] = staticFiles[url.pathname];
        res.writeHead(200, {
          'Content-Type': contentType,
          'Cache-Control': 'no-store',
          'X-Content-Type-Options': 'nosniff',
        });
        res.end(fs.readFileSync(path.join(ROOT, 'public', file)));
        return;
      }
      if (req.method === 'GET' && url.pathname === '/api/session') {
        send(res, 200, { token: sessionToken });
        return;
      }
      if (req.method === 'GET' && url.pathname === '/api/installed') {
        send(res, 200, await listInstalled(config), false);
        return;
      }
      if (req.method === 'GET' && url.pathname === '/api/state') {
        send(res, 200, {
          locale: config.locale,
          jobs: store.list().map(job => publicJob(job)),
          sessions: store.listSessions().map(session => publicSession(session)),
          providers: providerStates(config),
          projects: config.projects.map(({ id, label, path: projectPath }) => ({ id, label, path: projectPath })),
          connection,
          receiver: localizeStatus(receiver.summary(), config.locale),
          pairing: pairing.summary(),
          tunnel: publicTunnel(),
          voice: publicVoice(),
          models: publicModels(config),
        });
        return;
      }
      if (req.method === 'POST' && url.pathname.startsWith('/api/')) {
        if (!equalToken(req.headers['x-vibe-token'], sessionToken)) {
          send(res, 403, { error: '缺少本机操作令牌' });
          return;
        }
        if (url.pathname === '/api/settings/language') {
          send(res, 200, saveLanguageSettings(config, await body(req)));
          return;
        }
        if (url.pathname === '/api/sessions') {
          send(res, 201, publicSession(store.createSession(await body(req))));
          return;
        }
        const renameSession = url.pathname.match(/^\/api\/sessions\/([0-9a-f-]+)\/rename$/);
        if (renameSession) {
          send(res, 200, publicSession(store.renameSession(renameSession[1], (await body(req)).title)));
          return;
        }
        const deleteSession = url.pathname.match(/^\/api\/sessions\/([0-9a-f-]+)\/delete$/);
        if (deleteSession) {
          send(res, 200, store.deleteSession(deleteSession[1]));
          return;
        }
        if (url.pathname === '/api/jobs') {
          send(res, 201, publicJob(store.submit({ ...await body(req), source: 'desktop' })));
          return;
        }
        if (url.pathname === '/api/settings/openai-key') {
          let value;
          try {
            const payload = await body(req);
            value = payload.apiKey;
          } catch {
            throw new Error('密钥请求格式无效');
          }
          validateOpenAIKey(value);
          const selected = config.speech.pluginId;
          saveSpeechSettings(config, {pluginId: 'openai', settings: {apiKey: value}, clearSecrets: value ? [] : ['apiKey']});
          if (selected !== 'openai') saveSpeechSettings(config, {pluginId: selected, settings: {}});
          send(res, 200, {
            configured: Boolean(config.openaiKey),
            source: config.openaiKeySource,
            voice: publicVoice(),
          });
          return;
        }
        if (url.pathname === '/api/settings/voice') {
          const payload = await body(req);
          const voiceUrl = validateTranscribeUrl(payload.url);
          const model = validateTranscribeModel(payload.model);
          if (payload.clearKey !== undefined && typeof payload.clearKey !== 'boolean') {
            throw new Error('clearKey 必须为布尔值');
          }
          if (payload.apiKey !== undefined) validateOpenAIKey(payload.apiKey);
          if (payload.clearKey && payload.apiKey) {
            throw new Error('不能同时设置和清除语音 API Key');
          }
          saveSpeechSettings(config, {pluginId: 'openai', settings: {url: voiceUrl, model, ...(payload.apiKey !== undefined ? {apiKey: payload.apiKey} : {})}, clearSecrets: payload.clearKey ? ['apiKey'] : []});
          send(res, 200, { voice: publicVoice() });
          return;
        }
        if (url.pathname === '/api/settings/speech') {
          saveSpeechSettings(config, await body(req));
          send(res, 200, { voice: publicVoice() });
          return;
        }
        if (url.pathname === '/api/settings/models') {
          const models = saveModelSettings(config.stateDir, await body(req), config.pluginsRuntime);
          config.models = models;
          config.codexModel = models.codex;
          send(res, 200, { models: publicModels(config) });
          return;
        }
        if (url.pathname === '/api/tunnel/start') {
          send(res, 200, publicTunnel(await tunnel.start()));
          return;
        }
        if (url.pathname === '/api/tunnel/settings') {
          send(res, 200, publicTunnel(tunnel.configure(await body(req))));
          return;
        }
        if (url.pathname === '/api/tunnel/stop') {
          send(res, 200, publicTunnel(await tunnel.stop()));
          return;
        }
        if (url.pathname === '/api/pair/open') {
          send(res, 200, pairing.open());
          return;
        }
        if (url.pathname === '/api/pair/close') {
          send(res, 200, pairing.close());
          return;
        }
        const pairRemove = url.pathname.match(/^\/api\/pair\/([0-9a-f]{12})\/remove$/);
        if (pairRemove) {
          const removed = pairing.remove(pairRemove[1]);
          boardLink.forget(pairRemove[1]);
          send(res, 200, removed);
          return;
        }
        if (url.pathname === '/api/plugins/enabled') {
          const payload = await body(req);
          if (!payload || typeof payload !== 'object' || typeof payload.enabled !== 'boolean' || typeof payload.id !== 'string') {
            throw new Error('插件设置无效');
          }
          send(res, 200, await setPluginEnabled(config, payload.id, payload.enabled), false);
          return;
        }
        if (url.pathname === '/api/plugins/settings') {
          const payload = await body(req);
          if (!payload || typeof payload !== 'object' || typeof payload.id !== 'string' || !payload.settings || typeof payload.settings !== 'object' || Array.isArray(payload.settings)) {
            throw new Error('插件配置无效');
          }
          send(res, 200, await savePluginSettings(config, payload.id, payload.settings), false);
          return;
        }
        if (url.pathname === '/api/plugins/remove') {
          const payload = await body(req);
          send(res, 200, await removePlugin(config, {
            id: typeof payload?.id === 'string' ? payload.id : '',
            file: typeof payload?.file === 'string' ? payload.file : '',
          }), false);
          return;
        }
        const themeRemove = url.pathname.match(/^\/api\/themes\/([A-Za-z0-9_-]{1,64})\/remove$/);
        if (themeRemove) {
          send(res, 200, { themes: removeTheme(config, themeRemove[1]) }, false);
          return;
        }
        const themeApply = url.pathname.match(/^\/api\/themes\/([A-Za-z0-9_-]{1,64})\/apply$/);
        if (themeApply) {
          if (!themeExists(config, themeApply[1])) throw new Error('主题不存在');
          const paired = pairing.summary().paired;
          if (!paired.length) throw new Error('没有已配对的码得');
          let applied = 0;
          const failures = [];
          for (const device of paired) {
            try {
              boardLink.send(device.deviceId, { name: 'theme.apply', fields: { name: themeApply[1] } });
              applied += 1;
            } catch (error) {
              failures.push(error.message);
            }
          }
          if (!applied) throw new Error(failures[0] || '主题未能下发');
          send(res, 200, { applied, failed: failures.length }, false);
          return;
        }
        if (url.pathname === '/api/board/commands') {
          const wav = String(req.headers['content-type'] || '').startsWith('audio/wav');
          const deviceId = wav ? url.searchParams.get('deviceId') : '';
          if (wav) {
            if (typeof deviceId !== 'string' || !/^[0-9a-f]{12}$/.test(deviceId) || !pairing.paired.has(deviceId)) {
              throw new Error('设备未配对');
            }
            let audio;
            try {
              audio = await binaryBody(req, 256 * 1024);
            } catch (error) {
              if (error.message === '录音超过 30 秒上限') throw new Error('语音超过 256KB');
              throw error;
            }
            send(res, 200, { command: boardLink.send(deviceId, { name: 'audio.play' }, audio) });
            return;
          }
          const payload = await body(req);
          const named = payload && typeof payload === 'object' && !Array.isArray(payload) ? payload.deviceId : '';
          if (typeof named !== 'string' || !/^[0-9a-f]{12}$/.test(named) || !pairing.paired.has(named)) {
            throw new Error('设备未配对');
          }
          send(res, 200, { command: boardLink.send(named, { name: payload.name, fields: payload.fields }) });
          return;
        }
        const pairAction = url.pathname.match(/^\/api\/pair\/([0-9a-f]{12})\/(confirm|reject)$/);
        if (pairAction) {
          const { nonce } = await body(req);
          send(res, 200, pairing.decide(pairAction[1], nonce, pairAction[2]));
          return;
        }
        const match = url.pathname.match(/^\/api\/jobs\/([a-z0-9-]+)\/(confirm|cancel)$/);
        if (match) {
          send(res, 200, publicJob(match[2] === 'confirm' ? store.confirm(match[1]) : store.cancel(match[1])));
          return;
        }
      }
      send(res, 404, { error: '未找到接口' });
    } catch (error) {
      send(res, 400, { error: error.message }, !error.preserveMessage);
    }
  });
  const device = http.createServer(async (req, res) => {
    // Capture per request: device overrides never change the desktop setting.
    let deviceLocale = config.locale;
    const deviceSend = (res, status, value, translate = true) => send(res, status, value, translate, deviceLocale);
    try {
      const url = new URL(req.url, 'http://localhost');
      const requestedLocales = url.searchParams.getAll('uiLocale');
      if (requestedLocales.length > 1) throw new Error('语言仅支持 zh-CN 或 en');
      if (requestedLocales.length) deviceLocale = validateLocale(requestedLocales[0]);
      const address = req.socket.remoteAddress || '';
      const authority = normalizedAuthority(req.headers.host);
      const usbAuthority = [USB_RECEIVER_AUTHORITY, USB_DIRECT_AUTHORITY].includes(authority);
      const usbForwarded = address === '127.0.0.1' &&
        equalToken(req.headers['x-vibe-usb-forward'], usbForwardSecret) && usbAuthority;
      // These names identify an in-process serial forward, never a public
      // tunnel. Reject forged Hosts before pairing or issuing any credentials.
      if (usbAuthority && !usbForwarded) {
        deviceSend(res, 403, { error: 'USB 接入仅允许本机串口转发' });
        return;
      }
      if (req.method === 'GET' && url.pathname === '/device/tunnel-probe') {
        const nonce = url.searchParams.get('nonce');
        if (!/^[0-9a-f]{32}$/.test(nonce || '')) {
          deviceSend(res, 400, { error: '探针格式无效' });
        } else {
          deviceSend(res, 200, { nonce, bridgeId: pairing.bridgeId,
            authority: normalizedAuthority(req.headers.host) });
        }
        return;
      }
      // ---- 主题包（themes/<名字>/theme.json + 可选资产文件）----
      if (req.method === 'GET' && url.pathname === '/api/themes') {
        const themesDir = path.join(ROOT, 'themes');
        const out = [];
        try {
          for (const entry of fs.readdirSync(themesDir, { withFileTypes: true })) {
            if (!entry.isDirectory()) continue;
            const manifestPath = path.join(themesDir, entry.name, 'theme.json');
            if (!fs.existsSync(manifestPath)) continue;
            let title = entry.name;
            try {
              title = JSON.parse(fs.readFileSync(manifestPath, 'utf8')).title || title;
            } catch {}
            out.push({ name: entry.name, title });
          }
        } catch {}
        deviceSend(res, 200, out, false);
        return;
      }
      const themeManifestMatch =
        req.method === 'GET' && url.pathname.match(/^\/api\/themes\/([^/]+)$/);
      if (themeManifestMatch) {
        const name = themeManifestMatch[1];
        if (!/^[A-Za-z0-9_-]+$/.test(name)) {
          deviceSend(res, 400, { error: '主题名无效' });
          return;
        }
        const dir = path.join(ROOT, 'themes', name);
        const manifestPath = path.join(dir, 'theme.json');
        if (!fs.existsSync(manifestPath)) {
          deviceSend(res, 404, { error: '主题不存在' });
          return;
        }
        let title = name;
        try {
          title = JSON.parse(fs.readFileSync(manifestPath, 'utf8')).title || title;
        } catch {}
        const files = fs
          .readdirSync(dir)
          .filter((f) => fs.statSync(path.join(dir, f)).isFile());
        deviceSend(res, 200, { name, title, files }, false);
        return;
      }
      const themeFileMatch =
        req.method === 'GET' &&
        url.pathname.match(/^\/api\/themes\/([^/]+)\/files\/([A-Za-z0-9_.-]+)$/);
      if (themeFileMatch) {
        const [, themeName, themeFile] = themeFileMatch;
        if (!/^[A-Za-z0-9_-]+$/.test(themeName)) {
          deviceSend(res, 400, { error: '主题名无效' });
          return;
        }
        const filePath = path.join(ROOT, 'themes', themeName, themeFile);
        if (!fs.existsSync(filePath) || !fs.statSync(filePath).isFile()) {
          deviceSend(res, 404, { error: '文件不存在' });
          return;
        }
        res.writeHead(200, { 'Content-Type': 'application/octet-stream' });
        fs.createReadStream(filePath).pipe(res);
        return;
      }
      if (req.method === 'POST' && url.pathname === '/pair/request') {
        deviceSend(res, 202, pairing.request(await body(req), address, normalizedAuthority(req.headers.host) || '未知地址'));
        return;
      }
      if (req.method === 'GET' && url.pathname === '/pair/status') {
        deviceSend(res, 200, pairing.status(url.searchParams.get('deviceId'), url.searchParams.get('nonce'), address));
        return;
      }
      if (req.method === 'GET' && url.pathname === '/pair/verify') {
        const deviceId = url.searchParams.get('deviceId');
        const nonce = url.searchParams.get('nonce');
        if (!/^[0-9a-f]{12}$/.test(deviceId || '') || !/^[0-9a-f]{32}$/.test(nonce || '')) {
          deviceSend(res, 400, { error: '校验请求格式无效' });
          return;
        }
        const publicUrl = tunnel.summary().status === 'online' ? tunnel.summary().url : null;
        const publicAuthority = publicUrl ? new URL(publicUrl).host.toLowerCase() : null;
        if (!authority || (authority !== publicAuthority && !localAuthorities(config.devicePort).has(authority) &&
            !usbForwarded)) {
          deviceSend(res, 403, { error: '接入地址未获桥接器授权' });
          return;
        }
        const verified = pairing.verifyManual(deviceId, nonce, address, authority);
        deviceSend(res, verified ? 200 : 404, verified || { error: '设备未配对' });
        return;
      }
      const token = (req.headers.authorization || '').replace(/^Bearer\s+/i, '');
      if (!pairing.authenticate(token)) {
        deviceSend(res, 401, { error: '设备未授权' });
        return;
      }
      if (req.method === 'GET' && url.pathname === '/device/heartbeat') {
        if (process.env.VIBE_UI_DIAGNOSTICS === '1') {
          const fields = Object.fromEntries(['uiStage', 'uiAge', 'bootAge', 'exit', 'provider', 'displayStage', 'displayCount']
            .map((key) => [key, url.searchParams.get(key)])
            .filter(([, value]) => /^\d{1,10}$/.test(value || '')));
          if (Object.keys(fields).length) console.info('vibe-ui', JSON.stringify(fields));
        }
        deviceSend(res, 200, { status: 'ok', serverTime: Date.now() });
        return;
      }
      const deviceId = pairing.deviceIdForToken(token);
      if (req.method === 'POST' && url.pathname === '/device/capabilities') {
        boardLink.setCatalog(deviceId, await body(req));
        boardLink.connect(deviceId, config.pluginsRuntime, config);
        deviceSend(res, 200, { events: boardLink.subscription(deviceId, config.pluginsRuntime, config) });
        return;
      }
      if (req.method === 'POST' && url.pathname === '/device/events') {
        await boardLink.handleEvent(deviceId, await body(req), config.pluginsRuntime, config);
        deviceSend(res, 200, { accepted: true });
        return;
      }
      if (req.method === 'GET' && url.pathname === '/device/commands') {
        deviceSend(res, 200, { commands: boardLink.pending(deviceId) });
        return;
      }
      const commandAudio = req.method === 'GET' && url.pathname.match(/^\/device\/commands\/([1-9][0-9]{0,15})\/audio$/);
      if (commandAudio) {
        const wav = boardLink.audio(deviceId, commandAudio[1]);
        if (!wav) {
          deviceSend(res, 404, { error: '语音不存在' });
          return;
        }
        res.writeHead(200, {
          'Content-Type': 'audio/wav',
          'Content-Length': wav.length,
          'Cache-Control': 'no-store',
          'X-Content-Type-Options': 'nosniff',
        });
        res.end(wav);
        return;
      }
      if (req.method === 'POST' && url.pathname === '/device/commands/ack') {
        deviceSend(res, 200, boardLink.ack(deviceId, await body(req)));
        return;
      }
      if (req.method === 'GET' && url.pathname === '/device/tasks') {
        const provider = url.searchParams.get('provider');
        if (provider && !config.pluginsRuntime.hasAgent(provider)) throw new Error('未知编程工具');
        const sessionId = url.searchParams.get('sessionId');
        if (sessionId && (!VIBE_SESSION_ID.test(sessionId) || !store.sessions.get(sessionId))) {
          throw new Error('任务不存在');
        }
        deviceSend(res, 200, {
          // The board shows a session as one task. It only needs the latest
          // turn for its answer and BOOT confirm/cancel controls.
          jobs: (sessionId ? store.list().filter((job) => job.vibeSessionId === sessionId &&
            (!provider || job.provider === provider)) : []).slice(0, 1).map(job => publicJob(job, deviceLocale)).map((job) => {
            const result = deviceResultPreview(job.result, 1200);
            const instruction = devicePreview(job.instruction, 1200);
            return {
              id: job.id,
              vibeSessionId: job.vibeSessionId,
              instruction,
              instructionTruncated: Buffer.byteLength(deviceText(job.instruction)) > Buffer.byteLength(instruction),
              provider: job.provider,
              projectId: job.projectId,
              status: job.status,
              progress: devicePreview(job.progress, 160),
              result: result.text,
              resultTruncated: result.truncated,
              error: devicePreview(job.error.slice(-200), 96),
            };
          }),
        });
        return;
      }
      if (req.method === 'GET' && url.pathname === '/device/sessions') {
        const provider = url.searchParams.get('provider');
        if (provider && !config.pluginsRuntime.hasAgent(provider)) throw new Error('未知编程工具');
        // Old one-message jobs are preserved as desktop history. Only sessions
        // explicitly created after this feature was introduced appear on the
        // board, where choosing a conversation must be intentional.
        deviceSend(res, 200, { sessions: store.listSessions(provider).filter((session) => !session.legacy).map((session) => ({
          id: session.id, title: devicePreview(publicSession(session, deviceLocale).title, 120), provider: session.provider,
          projectId: session.projectId, status: session.status, updatedAt: session.updatedAt,
          contextMode: session.contextMode,
        })) });
        return;
      }
      if (req.method === 'POST' && url.pathname === '/device/sessions') {
        const provider = url.searchParams.get('provider') || config.defaultProvider;
        const projectId = url.searchParams.get('projectId') || config.defaultProject;
        if (!config.pluginsRuntime.hasAgent(provider)) throw new Error('未知编程工具');
        const payload = await body(req);
        const session = store.createSession({ provider, projectId, title: payload.title });
        deviceSend(res, 201, { ...session, title: devicePreview(publicSession(session, deviceLocale).title, 120) });
        return;
      }
      const deleteSession = url.pathname.match(/^\/device\/sessions\/([0-9a-f-]+)\/delete$/);
      if (req.method === 'POST' && deleteSession) {
        deviceSend(res, 200, store.deleteSession(deleteSession[1]));
        return;
      }
      if (req.method === 'GET' && url.pathname === '/device/config') {
        const voice = transcriptionState(config);
        // Firmware creates sessions in the default project; the full project
        // picker lives on the desktop. Keep room for twelve bounded bitmaps.
        const payload = {
          locale: config.locale,
          providers: providerStates(config, deviceLocale).map(({ id, label, available, reason, icon, capabilities }) => ({ id, label, available, reason: devicePreview(reason, 120), icon, capabilities })),
          projects: config.projects.filter(({ id }) => id === config.defaultProject).map(({ id, label }) => ({ id, label: devicePreview(label, 64) })),
          defaultProject: config.defaultProject,
          voice: { mode: voice.mode, available: voice.available },
          board: { events: boardLink.subscription(pairing.deviceIdForToken(token), config.pluginsRuntime, config) },
        };
        if (Buffer.byteLength(JSON.stringify(payload)) > 32 * 1024) throw new Error('码得配置超过 32 KB，请检查助手元数据');
        deviceSend(res, 200, payload);
        return;
      }
      if (req.method === 'POST' && url.pathname === '/device/voice') {
        if (voiceBusy) {
          deviceSend(res, 429, { error: '另一段录音正在识别，请稍后再试' });
          return;
        }
        if (!String(req.headers['content-type'] || '').startsWith('audio/wav')) {
          throw new Error('请上传 WAV 录音');
        }
        const sessionId = url.searchParams.get('sessionId');
        if (!VIBE_SESSION_ID.test(sessionId || '')) throw new Error('请先新建任务');
        const session = store.sessions.get(sessionId);
        if (!session) throw new Error('任务不存在');
        const provider = url.searchParams.get('provider') || session.provider;
        const projectId = url.searchParams.get('projectId') || session.projectId;
        if (!config.pluginsRuntime.hasAgent(provider)) throw new Error('未知编程工具');
        if (!config.projects.some((project) => project.id === projectId)) {
          throw new Error('项目不在本机允许列表中');
        }
        if (provider !== session.provider || projectId !== session.projectId) {
          throw new Error('任务与编程工具或项目不匹配');
        }
        voiceBusy = true;
        const controller = new AbortController();
        const disconnected = () => { if (!res.writableEnded) controller.abort(); };
        req.once('aborted', disconnected);
        res.once('close', disconnected);
        try {
          const wav = validateVoiceWav(await binaryBody(req, 1_100_000));
          controller.signal.throwIfAborted();
          const instruction = await (transcribe || ((audio, signal) => transcribeVoice(audio, config, signal)))(wav, controller.signal);
          // A recognizer may finish after disconnection even if it ignores abort.
          if (controller.signal.aborted || res.destroyed) return;
          const job = store.submit({ instruction, sessionId, provider, projectId, source: 'device_voice' });
          deviceSend(res, 201, { transcript: instruction, id: job.id, status: job.status,
            sessionId, vibeSessionId: sessionId });
        } catch (error) {
          if (!controller.signal.aborted) throw error;
        } finally {
          req.off('aborted', disconnected);
          res.off('close', disconnected);
          voiceBusy = false;
        }
        return;
      }
      const match = url.pathname.match(/^\/device\/tasks\/([a-z0-9-]+)\/(confirm|cancel)$/);
      if (req.method === 'POST' && match) {
        deviceSend(res, 200, publicJob(match[2] === 'confirm' ? store.confirm(match[1]) : store.cancel(match[1]), deviceLocale));
        return;
      }
      deviceSend(res, 404, { error: '未找到接口' });
    } catch (error) {
      deviceSend(res, error.message === '电脑配对窗口未开启' ? 403 :
        error.message === '校验请求过于频繁' ? 429 : 400, { error: error.message }, !error.preserveMessage);
    }
  });
  return {
    store,
    pairing,
    tunnel,
    receiver,
    connection,
    async listen() {
      try {
        await listenServer(desktop, config.port, '127.0.0.1');
        await listenServer(device, config.devicePort, '0.0.0.0');
        await new Promise((resolve, reject) => {
          discovery.once('error', reject);
          discovery.bind(discoveryPort, '0.0.0.0', () => {
            discovery.off('error', reject);
            resolve();
          });
        });
      } catch (error) {
        if (desktop.listening) await new Promise((resolve) => desktop.close(resolve));
        if (device.listening) await new Promise((resolve) => device.close(resolve));
        try { discovery.close(); } catch { /* Socket may not have bound. */ }
        throw error;
      }
      console.log(`电脑面板：http://127.0.0.1:${config.port}`);
      console.log(`码得通信接口已在端口 ${config.devicePort} 启用；局域网发现端口 ${discoveryPort}`);
      if (enableUsb || config.usbPort) receiver.start();
      // An Oray mapping can outlive this process. Read its actual state after
      // the device API is listening; this does not create or enable a mapping.
      if (typeof tunnel.refresh === 'function') {
        publicRefresh = Promise.resolve().then(() => tunnel.refresh()).catch(() => {});
      }
    },
    async close() {
      if (publicRefresh) await publicRefresh;
      await tunnel.stop();
      await receiver.stop();
      await new Promise((resolve) => desktop.close(resolve));
      await new Promise((resolve) => device.close(resolve));
      await new Promise((resolve) => discovery.close(resolve));
    },
  };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const config = loadConfig();
  config.pluginsRuntime = await loadConfiguredPlugins(config);
  const app = createApp(config, { enableUsb: true });
  let shuttingDown = false;
  const shutdown = () => {
    if (shuttingDown) return;
    shuttingDown = true;
    app.close().then(() => { process.exitCode = 0; }).catch((error) => {
      console.error(error.message);
      process.exitCode = 1;
    });
  };
  process.once('SIGINT', shutdown);
  process.once('SIGTERM', shutdown);
  process.once('exit', () => { try { app.tunnel.process?.kill('SIGTERM'); } catch { /* Already exited. */ } });
  app.listen().catch((error) => {
    console.error(error.message);
    process.exitCode = 1;
  });
}
