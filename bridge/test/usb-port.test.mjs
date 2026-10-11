import test from 'node:test';
import assert from 'node:assert/strict';
import { validateUsbPort } from '../src/config.mjs';

// Windows 上 USB 直连/接收器转发只有「手动填 VIBE_USB_PORT」这一条路，
// 而 COM3 在 Windows 上不是绝对路径。这条校验放错，Windows 用户就彻底走不通。

test('空值表示不指定端口', () => {
  assert.equal(validateUsbPort(''), '');
  assert.equal(validateUsbPort(undefined), '');
});

test('类 Unix 的绝对路径照收', () => {
  assert.equal(validateUsbPort('/dev/cu.usbmodem1101', 'darwin'), '/dev/cu.usbmodem1101');
  assert.equal(validateUsbPort('/dev/ttyUSB0', 'linux'), '/dev/ttyUSB0');
});

test('Windows 上传 COM 端口名（COM3 不是绝对路径）', () => {
  assert.equal(validateUsbPort('COM3', 'win32'), 'COM3');
  assert.equal(validateUsbPort('com12', 'win32'), 'com12');
  assert.equal(validateUsbPort('\\\\.\\COM10', 'win32'), '\\\\.\\COM10');
  assert.equal(validateUsbPort('COM10', 'win32'), 'COM10');
});

test('非 Windows 平台不接受裸 COM 名，避免误填当合法', () => {
  assert.throws(() => validateUsbPort('COM3', 'darwin'), /VIBE_USB_PORT/);
});

test('相对路径与控制字符一律拒绝', () => {
  assert.throws(() => validateUsbPort('ttyUSB0', 'linux'), /VIBE_USB_PORT/);
  assert.throws(() => validateUsbPort('../../etc/passwd', 'linux'), /VIBE_USB_PORT/);
  assert.throws(() => validateUsbPort('/dev/ttyUSB0\nX', 'linux'), /控制字符/);
  assert.throws(() => validateUsbPort('\u0000COM3', 'win32'), /控制字符/);
});
