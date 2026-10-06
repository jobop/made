import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import dgram from 'node:dgram';
import { createBoardLink } from '../src/board-link.mjs';
import { loadConfiguredPlugins } from '../src/plugins/registry.mjs';
import { loadConfig } from '../src/config.mjs';
import { createApp } from '../src/server.mjs';

const catalog = {
  events: [
    { name: 'boot.click', cadence: 'edge' },
    { name: 'imu.tilt', cadence: 'continuous' },
  ],
  commands: [{ name: 'caption.show' }, { name: 'audio.play' }, { name: 'volume.set' }],
};

function pcmWav() {
  const data = Buffer.alloc(16);
  const header = Buffer.alloc(44);
  header.write('RIFF', 0);
  header.writeUInt32LE(36 + data.length, 4);
  header.write('WAVE', 8);
  header.write('fmt ', 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20);
  header.writeUInt16LE(1, 22);
  header.writeUInt32LE(16000, 24);
  header.writeUInt32LE(32000, 28);
  header.writeUInt16LE(2, 32);
  header.writeUInt16LE(16, 34);
  header.write('data', 36);
  header.writeUInt32LE(data.length, 40);
  return Buffer.concat([header, data]);
}

function boardPlugin(extra = {}) {
  return {
    apiVersion: 1, kind: 'board-plugin', id: 'boot_caption', label: '按键回声',
    events: ['boot.click'], commands: ['caption.show'],
    probe: () => ({ available: true }),
    onEvent: () => ({ commands: [{ name: 'caption.show', fields: { text: '单击' } }] }),
    ...extra,
  };
}

test('只注册已激活插件和板侧清单的交集', async () => {
  const link = createBoardLink();
  const runtime = {
    boardPlugins: () => [
      boardPlugin(),
      boardPlugin({ id: 'tilt', events: ['imu.tilt', 'imu.shake'], commands: [], onEvent() {} }),
      boardPlugin({ id: 'asleep', probe: () => ({ available: false }), events: ['boot.click'] }),
    ],
  };
  assert.deepEqual(link.subscription('device', runtime, {}), []);
  link.setCatalog('device', catalog);
  assert.deepEqual(link.subscription('device', runtime, {}), ['boot.click', 'imu.tilt']);
  const handled = await link.handleEvent('device', { name: 'boot.click' }, runtime, {});
  assert.deepEqual(handled, { accepted: true });
  assert.deepEqual(link.pending('device'), [{ id: '1', name: 'caption.show', fields: { text: '单击' }, audio: false }]);
  await assert.rejects(link.handleEvent('device', { name: 'imu.shake' }, runtime, {}), /不在板侧清单/);
  const silent = { boardPlugins: () => [] };
  await assert.rejects(link.handleEvent('device', { name: 'boot.click' }, silent, {}), /未注册/);
});

test('插件不能下发清单外或自己未声明的命令', async () => {
  const link = createBoardLink();
  link.setCatalog('device', catalog);
  const runtime = {
    boardPlugins: () => [boardPlugin({
      onEvent: () => ({ commands: [
        { name: 'caption.show', fields: { text: '可以' } },
        { name: 'ui.scroll', fields: { direction: 'down' } },
        { name: 'caption.show', fields: { text: 1 } },
      ] }),
    })],
  };
  const handled = await link.handleEvent('device', { name: 'boot.click' }, runtime, {});
  assert.deepEqual(handled, { accepted: true });
  assert.deepEqual(link.pending('device'), [{ id: '1', name: 'caption.show', fields: { text: '可以' }, audio: false }]);
});

test('事件入队和桥接器直接下发互相独立', async () => {
  const link = createBoardLink();
  link.setCatalog('device', catalog);
  const runtime = {
    boardPlugins: () => [boardPlugin({
      async onEvent({ send }) {
        send({ name: 'caption.show', fields: { text: '插件' } });
        return { commands: [{ name: 'caption.show', fields: { text: '返回值' } }] };
      },
    })],
  };
  assert.deepEqual(await link.handleEvent('device', { name: 'boot.click' }, runtime, {}), { accepted: true });
  assert.deepEqual(link.pending('device').map((item) => item.fields.text), ['插件', '返回值']);
  const direct = link.send('device', { name: 'caption.show', fields: { text: '直接' } });
  assert.equal(direct.fields.text, '直接');
  assert.equal(link.pending('device').length, 3);
  assert.deepEqual(link.ack('device', { ids: ['1', '3'] }), { acked: 2 });
  assert.deepEqual(link.pending('device').map((item) => item.id), ['2']);
  assert.throws(() => link.send('other', { name: 'caption.show' }), /尚未上报/);
  assert.throws(() => link.send('device', { name: 'ui.scroll' }), /不在板侧清单/);
  link.forget('device');
  assert.deepEqual(link.pending('device'), []);
});

test('插件可以在事件处理返回后继续下发命令', async () => {
  const link = createBoardLink();
  link.setCatalog('device', catalog);
  let later = null;
  const runtime = {
    boardPlugins: () => [boardPlugin({
      onEvent({ send }) { later = send; },
    })],
  };
  assert.deepEqual(await link.handleEvent('device', { name: 'boot.click' }, runtime, {}), { accepted: true });
  assert.deepEqual(link.pending('device'), []);
  later({ name: 'caption.show', fields: { text: '稍后' } });
  assert.deepEqual(link.pending('device').map((item) => item.fields.text), ['稍后']);
  assert.throws(() => later({ name: 'ui.scroll' }), /不在板侧清单/);
  let blocked = null;
  const quiet = createBoardLink();
  quiet.setCatalog('device', catalog);
  await quiet.handleEvent('device', { name: 'boot.click' }, {
    boardPlugins: () => [boardPlugin({
      commands: [],
      onEvent({ send }) { blocked = send; },
    })],
  }, {});
  assert.throws(() => blocked({ name: 'caption.show', fields: { text: '不行' } }), /未声明/);
  assert.deepEqual(quiet.pending('device'), []);
});

test('设备上线时插件可以预约命令，不必先有事件', async () => {
  const link = createBoardLink();
  link.setCatalog('device', catalog);
  const wav = pcmWav();
  const runtime = {
    boardPlugins: () => [boardPlugin({
      commands: ['caption.show', 'audio.play'],
      onConnect({ send }) {
        send({ name: 'caption.show', fields: { text: '闹钟' } });
        send({ name: 'audio.play' }, wav);
      },
    })],
  };
  link.connect('device', runtime, {});
  assert.deepEqual(link.deviceIds(), ['device']);
  const pending = link.pending('device');
  assert.deepEqual(pending.map((item) => item.name), ['caption.show', 'audio.play']);
  assert.equal(pending[1].audio, true);
  assert.ok(link.audio('device', pending[1].id).equals(wav));
});

test('播放语音必须和命令一起入队，其他命令不能附带语音', async () => {
  const link = createBoardLink();
  link.setCatalog('device', catalog);
  const wav = pcmWav();
  assert.throws(() => link.send('device', { name: 'audio.play' }), /同时附带/);
  assert.throws(() => link.send('device', { name: 'volume.set', fields: { level: '40' } }, wav), /只有 audio.play/);
  const queued = link.send('device', { name: 'audio.play' }, wav);
  assert.equal(queued.audio, true);
  assert.equal(link.pending('device')[0].audio, true);
  assert.ok(link.audio('device', queued.id).equals(wav));
  link.ack('device', { ids: [queued.id] });
  assert.equal(link.audio('device', queued.id), null);
});

async function freeTcpPort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}

async function freeUdpPort() {
  const socket = dgram.createSocket('udp4');
  await new Promise((resolve, reject) => { socket.once('error', reject); socket.bind(0, '127.0.0.1', resolve); });
  const port = socket.address().port;
  socket.close();
  return port;
}

test('配对设备上报清单后，事件与命令分开传递', async (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-board-link-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  fs.writeFileSync(path.join(directory, 'caption.mjs'), `export default {
    apiVersion: 1, kind: 'board-plugin', id: 'boot_caption', label: '按键回声',
    events: ['boot.click', 'boot.long'], commands: ['caption.show'],
    probe: () => ({ available: true }),
    onEvent: ({ event }) => ({ commands: [{ name: 'caption.show', fields: { text: event.name } }] }),
  };`);
  const configPath = path.join(directory, 'config.json');
  fs.writeFileSync(configPath, JSON.stringify({
    projects: [{ id: 'demo', path: directory }],
    plugins: ['./caption.mjs'],
  }));
  const config = loadConfig({ VIBE_CONFIG: configPath, VIBE_STATE_DIR: path.join(directory, 'state'), VIBE_ASR_MODE: 'off' });
  config.pluginsRuntime = await loadConfiguredPlugins(config);
  config.port = await freeTcpPort();
  do { config.devicePort = await freeTcpPort(); } while (config.port === config.devicePort);
  config.discoveryPort = await freeUdpPort();
  const app = createApp(config);
  await app.listen();
  t.after(() => app.close());
  const device = `http://127.0.0.1:${config.devicePort}`;
  const deviceId = 'aabbccddeeff';
  const nonce = 'b'.repeat(32);
  app.pairing.open();
  app.pairing.request({ deviceId, deviceName: '测试圆屏', code: '654321', nonce }, '127.0.0.1', `127.0.0.1:${config.devicePort}`);
  app.pairing.decide(deviceId, nonce, 'confirm');
  const headers = { Authorization: `Bearer ${app.pairing.paired.get(deviceId).token}`, 'content-type': 'application/json' };
  const before = await (await fetch(`${device}/device/config`, { headers })).json();
  assert.deepEqual(before.board.events, []);
  const posted = await fetch(`${device}/device/capabilities`, {
    method: 'POST', headers, body: JSON.stringify({
      events: [{ name: 'boot.click', cadence: 'edge' }, { name: 'boot.long', cadence: 'edge' }, { name: 'imu.tilt', cadence: 'continuous' }],
      commands: [{ name: 'caption.show' }, { name: 'audio.play' }, { name: 'volume.set' }],
    }),
  });
  assert.equal(posted.status, 200);
  assert.deepEqual((await posted.json()).events, ['boot.click', 'boot.long']);
  const after = await (await fetch(`${device}/device/config`, { headers })).json();
  assert.deepEqual(after.board.events, ['boot.click', 'boot.long']);
  const reported = await fetch(`${device}/device/events`, {
    method: 'POST', headers, body: JSON.stringify({ name: 'boot.click' }),
  });
  assert.equal(reported.status, 200);
  assert.deepEqual(await reported.json(), { accepted: true });
  const queued = await (await fetch(`${device}/device/commands`, { headers })).json();
  assert.deepEqual(queued.commands, [{ id: '1', name: 'caption.show', fields: { text: 'boot.click' }, audio: false }]);
  const acked = await fetch(`${device}/device/commands/ack`, {
    method: 'POST', headers, body: JSON.stringify({ ids: ['1'] }),
  });
  assert.equal(acked.status, 200);
  assert.deepEqual(await acked.json(), { acked: 1 });
  assert.deepEqual((await (await fetch(`${device}/device/commands`, { headers })).json()).commands, []);
  const session = await (await fetch(`http://127.0.0.1:${config.port}/api/session`)).json();
  const direct = await fetch(`http://127.0.0.1:${config.port}/api/board/commands`, {
    method: 'POST',
    headers: { 'content-type': 'application/json', 'x-vibe-token': session.token },
    body: JSON.stringify({ deviceId, name: 'caption.show', fields: { text: '直接' } }),
  });
  assert.equal(direct.status, 200);
  assert.equal((await direct.json()).command.fields.text, '直接');
  assert.equal((await (await fetch(`${device}/device/commands`, { headers })).json()).commands.length, 1);
  const wav = pcmWav();
  const spoken = await fetch(`http://127.0.0.1:${config.port}/api/board/commands?deviceId=${deviceId}`, {
    method: 'POST',
    headers: { 'content-type': 'audio/wav', 'x-vibe-token': session.token },
    body: wav,
  });
  assert.equal(spoken.status, 200);
  const spokenId = (await spoken.json()).command.id;
  const listed = await (await fetch(`${device}/device/commands`, { headers })).json();
  assert.equal(listed.commands.find((item) => item.id === spokenId).audio, true);
  const clip = await fetch(`${device}/device/commands/${spokenId}/audio`, { headers });
  assert.equal(clip.status, 200);
  assert.equal(clip.headers.get('content-type'), 'audio/wav');
  assert.ok(Buffer.from(await clip.arrayBuffer()).equals(wav));
  const tilt = await fetch(`${device}/device/events`, {
    method: 'POST', headers, body: JSON.stringify({ name: 'imu.tilt', fields: { direction: 'down' } }),
  });
  assert.equal(tilt.status, 400);
  assert.match((await tilt.json()).error, /未注册/);
});
