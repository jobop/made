import test from 'node:test';
import assert from 'node:assert/strict';
import { parseWindowsPortNames } from '../../tools/flash.mjs';

// 刷机工具的 Windows 分支：COM10 及以上必须写成 \\.\COM10 才打得开，
// 这条规则错了会表现为「端口存在但打不开」，很难查，所以单独测。

test('Windows 端口名优先取 PowerShell 的结果', () => {
  assert.deepEqual(
    parseWindowsPortNames('COM3\r\nCOM10\r\n', 'COM3 REG_SZ COM3'),
    ['COM3', '\\\\.\\COM10'],
  );
});

test('PowerShell 没有结果时退回注册表输出', () => {
  const registry = [
    'HKEY_LOCAL_MACHINE\\HARDWARE\\DEVICEMAP\\SERIALCOMM',
    '    \\Device\\Serial0    REG_SZ    COM5',
    '    \\Device\\VCP0    REG_SZ    COM12',
    '',
  ].join('\r\n');
  assert.deepEqual(parseWindowsPortNames('', registry), ['\\\\.\\COM12', 'COM5']);
});

test('忽略非 COM 名与重复项', () => {
  assert.deepEqual(
    parseWindowsPortNames('COM4\n\nCOM4\n蓝牙串行端口\n', ''),
    ['COM4'],
  );
});

test('两条路都没有结果时返回空数组', () => {
  assert.deepEqual(parseWindowsPortNames('', ''), []);
});
