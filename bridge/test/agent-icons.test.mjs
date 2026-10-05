import test from 'node:test';
import assert from 'node:assert/strict';
import { builtinAgentIcons, rgbaToIndexed4, normalizeAgentIcon, validateIndexedIcon, MAX_ICON_JSON_BYTES } from '../src/agent-icons.mjs';
import { createPluginRegistry } from '../src/plugins/registry.mjs';
import { providerStates } from '../src/providers.mjs';

const agent = (id, icon) => ({
  apiVersion: 1, kind: 'coding-agent', id, label: `测试 ${id}`, icon,
  capabilities: { session: 'native', model: false, cancel: true, progress: true },
  probe: () => ({ available: true }), run: async () => ({ status: 'completed', result: '完成' }),
});
const customIcon = () => ({
  format: 'indexed4', width: 3, height: 1,
  palette: ['00000000', 'ff0000ff', '00ff0080'], data: Buffer.from([0x12, 0x10]).toString('base64'),
  background: '#012345', accent: '#abcdef',
});
function unpack(icon) {
  const bytes = Buffer.from(icon.data, 'base64');
  return Array.from({ length: icon.width * icon.height }, (_, i) => icon.palette[i % 2 ? bytes[i >> 1] & 15 : bytes[i >> 1] >> 4]);
}

test('四个内置头像是真实、不同的像素对象，旧别名只在桥接器解析', () => {
  const icons = ['fox', 'rabbit', 'owl', 'panda'].map(name => normalizeAgentIcon(name));
  assert.equal(new Set(icons.map(icon => icon.data)).size, 4);
  for (const icon of icons) {
    assert.equal(icon.format, 'indexed4');
    assert.equal(icon.width, 48);
    assert.equal(icon.height, 48);
    assert.equal(validateIndexedIcon(icon), icon);
    assert.equal(icon.palette.some(color => color.endsWith('00')), true);
    assert.ok(Buffer.byteLength(JSON.stringify(icon)) <= MAX_ICON_JSON_BYTES);
    assert.ok(Object.isFrozen(icon) && Object.isFrozen(icon.palette));
  }
  assert.equal(normalizeAgentIcon(undefined), builtinAgentIcons.generic);
  assert.notEqual(normalizeAgentIcon(undefined), builtinAgentIcons.fox);
  assert.throws(() => normalizeAgentIcon('unregistered-animal'));
});

test('任意第五个助手可直接下发像素，无需固件预设名称', () => {
  const supplied = customIcon();
  const registry = createPluginRegistry([
    ...['fox', 'rabbit', 'owl', 'panda'].map(name => agent(`preset_${name}`, name)),
    agent('fifth_custom', supplied),
  ]);
  supplied.palette[1] = 'ffffffff';
  const providers = providerStates({ pluginsRuntime: registry });
  assert.deepEqual(providers[4].icon, customIcon());
  assert.deepEqual(unpack(providers[4].icon), ['ff0000ff', '00ff0080', 'ff0000ff']);
  assert.equal(providers.every(provider => provider.icon.format === 'indexed4'), true);
  assert.equal(providers[4].id, 'fifth_custom');
});

test('图标校验拒绝超限尺寸、色表、Base64、越界索引和奇数像素填充', () => {
  const valid = customIcon();
  for (const icon of [
    { ...valid, format: 'png' }, { ...valid, width: 49 }, { ...valid, height: 0 },
    { ...valid, width: 1.5 }, { ...valid, height: Infinity },
    { ...valid, palette: [] }, { ...valid, palette: new Array(3) }, { ...valid, palette: Array(17).fill('ffffffff') },
    { ...valid, palette: ['#ff0000ff'] }, { ...valid, palette: [0] },
    { ...valid, background: 'red' }, { ...valid, accent: '#abc' },
    { ...valid, data: '' }, { ...valid, data: 'EhA=\n' }, { ...valid, data: 'EhB=' },
    { ...valid, data: Buffer.from([0x31, 0x10]).toString('base64') },
    { ...valid, data: Buffer.from([0x12, 0x11]).toString('base64') },
    { ...valid, data: 'A'.repeat(200000) }, { ...valid, url: 'https://example.invalid/icon' },
  ]) assert.throws(() => validateIndexedIcon(icon));
});

test('RGBA 生成器保留透明度与精确颜色，超过 16 色时确定性量化并限制体积', () => {
  const rgba = new Uint8Array([255, 0, 0, 255, 0, 255, 0, 128, 12, 34, 56, 0]);
  const exact = rgbaToIndexed4({ width: 3, height: 1, pixels: rgba });
  assert.deepEqual(unpack(exact), ['ff0000ff', '00ff0080', '00000000']);
  const gradient = new Uint8Array(48 * 48 * 4);
  for (let y = 0; y < 48; y++) for (let x = 0; x < 48; x++) gradient.set([x * 5, y * 5, (x + y) * 2, 255], (y * 48 + x) * 4);
  const args = { width: 48, height: 48, pixels: gradient };
  const icon = rgbaToIndexed4(args);
  assert.equal(icon.palette.length, 16);
  assert.deepEqual(rgbaToIndexed4(args), icon);
  assert.ok(Buffer.byteLength(JSON.stringify(icon)) < 2048);
  assert.throws(() => rgbaToIndexed4({ width: 49, height: 1, pixels: new Uint8Array(49 * 4) }));
  assert.throws(() => rgbaToIndexed4({ width: 3, height: 1, pixels: rgba.slice(1) }));
});

test('完整 12 助手的图标与有界名称能力数据可装入 32 KB 配置响应', () => {
  const icon = { ...builtinAgentIcons.fox, palette: [...builtinAgentIcons.fox.palette] };
  while (icon.palette.length < 16) icon.palette.push('ffffffff');
  const registry = createPluginRegistry(Array.from({ length: 12 }, (_, i) => ({
    ...agent(`agent_${i}`.padEnd(40, 'x'), icon), label: 'n'.repeat(64),
    probe: () => ({ available: false, reason: '测'.repeat(120) }),
  })));
  const providers = providerStates({ pluginsRuntime: registry }).map(({ id, label, icon: image, available, reason, capabilities }) => ({ id, label, icon: image, available, reason: [...reason].slice(0, 120).join(''), capabilities }));
  // Device config additionally has a 512-character project path, identifiers,
  // transport metadata and flags; keep an explicit reserve for those fields.
  const config = { providers, project: 'p'.repeat(64), projectPath: '/'.repeat(512), protocolVersion: 1 };
  assert.ok(Buffer.byteLength(JSON.stringify(config)) + 2048 < 32768);
});
