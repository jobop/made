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
  commands: [{ name: 'caption.show' }],
};

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
  assert.deepEqual(handled.commands, [{ name: 'caption.show', fields: { text: '单击' } }]);
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
  assert.deepEqual(handled.commands, [{ name: 'caption.show', fields: { text: '可以' } }]);
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

test('配对设备上报清单后，配置只下发已激活事件，事件换回命令', async (t) => {
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
      commands: [{ name: 'caption.show' }],
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
  assert.deepEqual(await reported.json(), { commands: [{ name: 'caption.show', fields: { text: 'boot.click' } }] });
  const tilt = await fetch(`${device}/device/events`, {
    method: 'POST', headers, body: JSON.stringify({ name: 'imu.tilt', fields: { direction: 'down' } }),
  });
  assert.equal(tilt.status, 400);
  assert.match((await tilt.json()).error, /未注册/);
});
