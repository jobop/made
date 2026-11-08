import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import dgram from 'node:dgram';
import http from 'node:http';
import { JobStore } from '../src/jobs.mjs';
import { createApp } from '../src/server.mjs';
import { createPluginRegistry } from '../src/plugins/registry.mjs';
import { codexFixture, testRegistry } from './helpers/fixture-agents.mjs';
import {
  MAX_IMAGE_BYTES, saveImage, readImage, resolveTaskImages,
  sanitizeInputImages, withImagePrompt, absolutizeInputImages,
} from '../src/device-images.mjs';

// 只需要合法文件头：桥接器按魔数判格式，不解码像素。
const PNG = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 0]);
const JPEG = Buffer.concat([Buffer.from([0xff, 0xd8, 0xff, 0xe0]), Buffer.alloc(24)]);
const SESSION_ID = '11111111-2222-3333-4444-555555555555';

function tmpDir() {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-images-'));
}

async function until(check) {
  const deadline = Date.now() + 1500;
  while (!check()) {
    if (Date.now() > deadline) throw new Error('超时');
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
}

test('图片落盘到会话目录、按魔数校验、读回与编号白名单', (t) => {
  const dir = tmpDir();
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const saved = saveImage(dir, SESSION_ID, PNG, '../照片/截图.png');
  assert.match(saved.id, /^[a-f0-9]{12}$/);
  assert.equal(saved.name, '__照片_截图.png');
  assert.equal(saved.file, `uploads/${saved.id}.png`);
  assert.equal(saved.url, `/device/images/${saved.id}?sessionId=${SESSION_ID}`);
  assert.ok(fs.existsSync(path.join(dir, saved.file)));

  const read = readImage(dir, saved.id);
  assert.equal(read.mime, 'image/png');
  assert.deepEqual(read.bytes, PNG);
  assert.equal(readImage(dir, 'abcdefghijkl'), null, '非法 id 不读');
  assert.equal(readImage(dir, '../jobs'), null, '路径穿越不读');

  assert.throws(() => saveImage(dir, SESSION_ID, Buffer.from('not an image'), 'x.png'), /只支持/);
  assert.throws(() => saveImage(dir, SESSION_ID, Buffer.alloc(MAX_IMAGE_BYTES + 1, 0), 'x.png'), /6MB/);
  assert.throws(() => saveImage(dir, SESSION_ID, Buffer.alloc(0), 'x.png'), /为空/);
  const jpeg = saveImage(dir, SESSION_ID, JPEG, 'photo.jpg');
  assert.equal(jpeg.mime, 'image/jpeg');
  assert.equal(jpeg.file, `uploads/${jpeg.id}.jpg`);
});

test('任务图片 id 必须在同一会话且已落盘', (t) => {
  const dir = tmpDir();
  const other = tmpDir();
  t.after(() => { fs.rmSync(dir, { recursive: true, force: true }); fs.rmSync(other, { recursive: true, force: true }); });
  const saved = saveImage(dir, SESSION_ID, PNG, 'a.png');
  assert.equal(resolveTaskImages(dir, SESSION_ID, [saved.id]).length, 1);
  assert.deepEqual(resolveTaskImages(dir, SESSION_ID, []), []);
  assert.throws(() => resolveTaskImages(dir, SESSION_ID, ['../jobs']), /编号无效/);
  assert.throws(() => resolveTaskImages(other, SESSION_ID, [saved.id]), /不存在/);
  assert.throws(() => resolveTaskImages(dir, SESSION_ID, [saved.id, saved.id, saved.id, saved.id, saved.id]), /最多/);
  assert.deepEqual(sanitizeInputImages([{ id: 'bad', file: 'x' }, { id: saved.id, file: saved.file }]),
    [{ id: saved.id, name: '', url: '', file: saved.file }]);
});

test('图片提示只加在执行副本，存储里的用户指令保持原文', async (t) => {
  const dir = tmpDir();
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  let executedInstruction = '';
  let executedImages = null;
  const store = new JobStore({
    file: path.join(dir, 'jobs.json'),
    projects: [{ id: 'demo', label: 'Demo', path: dir }],
    defaultProject: 'demo', defaultProvider: 'codex',
    workspaceRoot: path.join(dir, 'ws'),
    pluginsRuntime: createPluginRegistry([codexFixture]),
    providerAvailable: () => ({ available: true }),
    run: async (job) => {
      executedInstruction = job.instruction;
      executedImages = job.inputImages;
      return { status: 'completed', result: '收到' };
    },
  });
  const session = store.createSession({ provider: 'codex' });
  const saved = saveImage(store.workspacePath(session.id), session.id, PNG, 'a.png');
  const job = store.submit({
    instruction: '这是什么颜色', sessionId: session.id, provider: 'codex',
    projectId: 'demo', source: 'device_image',
    inputImages: [{ id: saved.id, name: saved.name, url: saved.url, file: saved.file }],
  });
  assert.equal(job.inputImages.length, 1);
  assert.equal(job.instruction, '这是什么颜色');
  store.confirm(job.id);
  await until(() => job.status === 'completed');
  // 多模态直传：提示只说「图已附上」，不带读文件路径；图片本体以绝对路径
  // + mime 随执行副本透传给插件（插件转成 ImageContent 原生带给模型）。
  assert.match(executedInstruction, /^这是什么颜色/);
  assert.match(executedInstruction, /已随本条消息附上/);
  assert.doesNotMatch(executedInstruction, /uploads\//);
  assert.equal(job.instruction, '这是什么颜色', '执行副本不得回写存储');
  assert.equal(withImagePrompt('纯文本', []), '纯文本');
  assert.equal(executedImages.length, 1);
  assert.equal(executedImages[0].mime, 'image/png');
  assert.equal(executedImages[0].file, path.resolve(store.workspacePath(session.id), saved.file));
  assert.ok(executedImages[0].bytes > 0);
});

test('absolutizeInputImages：越界/缺文件/未知格式剔除，无图返回 undefined', (t) => {
  const dir = tmpDir();
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const ok = saveImage(dir, SESSION_ID, PNG, 'a.png');
  const out = absolutizeInputImages(dir, [
    { id: ok.id, file: ok.file, name: 'a.png' },
    { id: 'aaaaaaaaaaaa', file: '../../escape.png', name: 'x' },
    { id: 'bbbbbbbbbbbb', file: 'uploads/missing.png', name: 'y' },
    { id: 'cccccccccccc', file: 'uploads/cccccccccccc.txt', name: 'z' },
  ]);
  assert.equal(out.length, 1);
  assert.equal(out[0].id, ok.id);
  assert.equal(path.dirname(out[0].file), path.resolve(dir, 'uploads'));
  assert.equal(absolutizeInputImages(dir, []), undefined);
  assert.equal(absolutizeInputImages('', [{ id: ok.id, file: ok.file }]), undefined);
});

async function freeTcpPort() {
  const server = net.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const port = server.address().port;
  await new Promise((resolve) => server.close(resolve));
  return port;
}

async function freeUdpPort() {
  const socket = dgram.createSocket('udp4');
  await new Promise((resolve, reject) => {
    socket.once('error', reject);
    socket.bind(0, '127.0.0.1', resolve);
  });
  const port = socket.address().port;
  await new Promise((resolve) => socket.close(resolve));
  return port;
}

async function request(port, pathname, { method = 'GET', token, json, bytes, contentType } = {}) {
  const headers = {};
  if (token) headers.authorization = `Bearer ${token}`;
  if (json !== undefined) headers['content-type'] = 'application/json';
  if (bytes !== undefined) headers['content-type'] = contentType || 'application/octet-stream';
  const response = await fetch(`http://127.0.0.1:${port}${pathname}`, {
    method,
    headers,
    body: json !== undefined ? JSON.stringify(json) : bytes,
  });
  const buf = Buffer.from(await response.arrayBuffer());
  let data = null;
  try { data = JSON.parse(buf.toString('utf8')); } catch { /* 二进制 */ }
  return { status: response.status, buf, data, type: response.headers.get('content-type') };
}

test('设备接口：APP 传图 → 回读 → 带图提交任务 → 板端白名单回显', async (t) => {
  const stateDir = tmpDir();
  const devicePort = await freeTcpPort();
  const config = {
    port: await freeTcpPort(), devicePort, discoveryPort: await freeUdpPort(),
    stateDir, workspaceRoot: path.join(stateDir, 'ws'),
    projects: [{ id: 'demo', label: 'Demo', path: stateDir }],
    defaultProject: 'demo', defaultProvider: 'codex', asrMode: 'off',
    workbuddy: { allowUnscopedDispatch: false }, pluginsRuntime: testRegistry(),
  };
  const app = createApp(config, { run: async () => ({ status: 'completed', result: 'done' }) });
  await app.listen();
  t.after(async () => { await app.close(); fs.rmSync(stateDir, { recursive: true, force: true }); });
  const deviceId = '0123456789ab';
  const nonce = 'a'.repeat(32);
  app.pairing.open();
  app.pairing.request({ deviceId, deviceName: '手机', code: '123456', nonce },
    '127.0.0.1', `127.0.0.1:${devicePort}`);
  app.pairing.decide(deviceId, nonce, 'confirm');
  const token = app.pairing.paired.get(deviceId).token;

  const session = await request(devicePort, '/device/sessions?provider=codex', {
    method: 'POST', token, json: {},
  });
  assert.equal(session.status, 201);
  const sessionId = session.data.id;
  assert.ok(sessionId);

  assert.equal((await request(devicePort, `/device/images?sessionId=${sessionId}`, {
    method: 'POST', bytes: PNG, contentType: 'image/png',
  })).status, 401, '未授权不能传图');

  const upload = await request(devicePort,
    `/device/images?sessionId=${sessionId}&name=${encodeURIComponent('截图.png')}`, {
      method: 'POST', token, bytes: PNG, contentType: 'image/png',
    });
  assert.equal(upload.status, 201, JSON.stringify(upload.data));
  assert.equal(upload.data.name, '截图.png');
  assert.equal(upload.data.file, undefined, '不回传服务端文件路径');
  const id = upload.data.id;

  const readBack = await request(devicePort, `/device/images/${id}?sessionId=${sessionId}`, { token });
  assert.equal(readBack.status, 200);
  assert.equal(readBack.type, 'image/png');
  assert.deepEqual(readBack.buf, PNG);

  const job = await request(devicePort, '/device/tasks', {
    method: 'POST', token, json: { instruction: '这是什么', sessionId, images: [id] },
  });
  assert.equal(job.status, 201, JSON.stringify(job.data));
  assert.equal(job.data.source, 'device_image');
  assert.equal(job.data.inputImages.length, 1);
  assert.equal(job.data.inputImages[0].id, id);

  const board = await request(devicePort, `/device/tasks?provider=codex&sessionId=${sessionId}`, { token });
  assert.equal(board.status, 200);
  assert.equal(board.data.jobs.length, 1);
  assert.equal(board.data.jobs[0].instruction, '这是什么');
  assert.equal(board.data.jobs[0].inputImages[0].url, upload.data.url, 'APP 可回显');
  assert.deepEqual(board.data.jobs[0].attachments, [], '输出附件不受影响');

  // 只发图不打字：服务端补一句默认指令
  const noText = await request(devicePort, '/device/tasks', {
    method: 'POST', token, json: { sessionId, images: [id] },
  });
  // 同一会话已有 waiting_confirmation 任务，第二条会被拒；先取消第一条再试。
  assert.equal(noText.status, 400);
  await request(devicePort, `/device/tasks/${job.data.id}/cancel`, { method: 'POST', token, json: {} });
  const imageOnly = await request(devicePort, '/device/tasks', {
    method: 'POST', token, json: { sessionId, images: [id] },
  });
  assert.equal(imageOnly.status, 201, JSON.stringify(imageOnly.data));
  assert.match(imageOnly.data.instruction, /请查看我发的图片/);

  // 跨会话的 id 不认（图片属于会话工作目录）
  const second = await request(devicePort, '/device/sessions?provider=codex', {
    method: 'POST', token, json: {},
  });
  assert.equal(second.status, 201);
  const cross = await request(devicePort, '/device/tasks', {
    method: 'POST', token, json: { instruction: '跨会话测试', sessionId: second.data.id, images: [id] },
  });
  assert.equal(cross.status, 400);
  assert.match(cross.data.error, /不存在|清理/);
});


test('标准 WebP 文件头可上传，拼错的 FourCC 不接受', (t) => {
  const dir = tmpDir();
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const bytes = Buffer.concat([Buffer.from('RIFF'), Buffer.from([12, 0, 0, 0]), Buffer.from('WEBPVP8 '), Buffer.alloc(4)]);
  const saved = saveImage(dir, SESSION_ID, bytes, 'photo.webp');
  assert.equal(saved.mime, 'image/webp');
  assert.deepEqual(readImage(dir, saved.id).bytes, bytes);
  assert.equal(resolveTaskImages(dir, SESSION_ID, [saved.id])[0].file, saved.file);
  assert.throws(() => saveImage(dir, SESSION_ID, Buffer.from('RIFFxxxxWEBVPxxxx'), 'bad.webp'), /只支持/);
});

test('上传后被清理的图片不会让执行指令声称已附上图片', async (t) => {
  const dir = tmpDir();
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  let received;
  const store = new JobStore({
    file: path.join(dir, 'jobs.json'),
    projects: [{ id: 'demo', path: dir }],
    defaultProject: 'demo', defaultProvider: 'codex',
    workspaceRoot: path.join(dir, 'ws'),
    pluginsRuntime: testRegistry(),
    providerAvailable: () => ({ available: true }),
    run: async (job) => { received = job; return { status: 'completed', result: 'ok' }; },
  });
  const session = store.createSession({ provider: 'codex' });
  const image = saveImage(store.workspacePath(session.id), session.id, PNG, 'removed.png');
  const job = store.submit({ instruction: '检查图片', sessionId: session.id, inputImages: [image] });
  fs.unlinkSync(path.join(store.workspacePath(session.id), image.file));
  store.confirm(job.id);
  await until(() => job.status === 'completed');
  assert.equal(received.inputImages, undefined);
  assert.equal(received.instruction, '检查图片');
  assert.equal(job.inputImages.length, 1, '原任务记录仍保留附件信息');
});
