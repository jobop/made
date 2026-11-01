import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import childProcess from 'node:child_process';
import { syncBuiltinESMExports } from 'node:module';

// 纯决策函数：只在「原生 ESP 口 + 线上完全静默」时按节拍升级。
test('helloEscalation 决策矩阵', async () => {
  const { helloEscalation } = await import('../src/usb-receiver.mjs?esc-pure');
  // 非 3 的倍数或零：只释放。
  assert.equal(helloEscalation(0, {}), 'release');
  assert.equal(helloEscalation(1, { native: true }), 'release');
  assert.equal(helloEscalation(2, { native: true, silent: true }), 'release');
  assert.equal(helloEscalation(4, { native: true }), 'release');
  // 3/6/9 次静默：复位。
  for (const count of [3, 6, 9]) {
    assert.equal(helloEscalation(count, { native: true, silent: true }), 'reset');
  }
  // 12 次起：放弃复位。
  assert.equal(helloEscalation(12, { native: true, silent: true }), 'giveup');
  assert.equal(helloEscalation(15, { native: true, silent: true }), 'giveup');
  // 线上有数据（健康但未启用 USB 的设备在打日志）：永不复位。
  assert.equal(helloEscalation(9, { native: true, silent: false }), 'release');
  assert.equal(helloEscalation(12, { native: true, silent: false }), 'release');
  // 非原生口（WCH 接收器）：stty 不是复位，永不升级。
  assert.equal(helloEscalation(9, { native: false, silent: true }), 'release');
});

for (const scenario of ['release-then-reset', 'giveup', 'no-escalation-with-logs', 'no-escalation-non-native']) {
  test(`握手超时升级恢复，${scenario}`,
    { skip: !['darwin', 'linux'].includes(process.platform), timeout: 5000 }, async (t) => {
      const port = '/dev/cu.vibe-esc-test';
      const fd = 7654321;
      const sttyCalls = [];
      const execFile = childProcess.execFile;
      const close = fs.close;
      childProcess.execFile = (command, args, options, callback) => {
        if (command === 'stty') {
          sttyCalls.push(args);
          queueMicrotask(() => callback(null, '', ''));
          return;
        }
        queueMicrotask(() => callback(new Error('unexpected command'), '', ''));
      };
      fs.close = (value, callback) => {
        if (value !== fd) return close(value, callback);
        queueMicrotask(() => callback(null));
      };
      syncBuiltinESMExports();
      t.after(() => {
        childProcess.execFile = execFile;
        fs.close = close;
        syncBuiltinESMExports();
      });
      const { UsbReceiver } = await import(`../src/usb-receiver.mjs?esc-test=${scenario}`);
      const native = !scenario.includes('non-native');
      const receiver = new UsbReceiver({ devicePort: 1 });
      // 伪造已打开的链路：直连原生 ESP 口、线上静默（或按场景带日志流量）。
      const link = { fd, port, usbId: native ? '303a:1001' : '1a86:55d3',
        sawInput: scenario === 'no-escalation-with-logs',
        helloTimer: null, inputTimer: null, protocol: null, writeQueue: Promise.resolve() };
      receiver.links.set(port, link);
      receiver.link = link;
      receiver.state = { ...receiver.state, status: 'waiting', port };

      const resetCount = scenario === 'giveup' ? 12 : scenario === 'release-then-reset' ? 3 : 9;
      for (let round = 1; round <= resetCount; round += 1) {
        // disconnect 会把 link 从 links 里摘掉，逐轮重新挂回去模拟重开。
        receiver.handleHelloTimeout(link, port);
        if (receiver.links.get(port) !== link) receiver.links.set(port, link);
      }

      const expectedResets = scenario === 'release-then-reset' ? 1
        : scenario === 'giveup' ? 3 : 0;
      assert.equal(sttyCalls.length, expectedResets);
      for (const args of sttyCalls) {
        assert.equal(args[0], process.platform === 'darwin' ? '-f' : '-F');
        assert.equal(args[1], port);
        assert.ok(args.includes('921600') && args.includes('raw'));
      }
      assert.equal(receiver.helloFailures.get(port), resetCount);
      if (scenario === 'release-then-reset') {
        // 第 3 次失败触发复位并留启动窗口冷却。
        assert.ok(receiver.cooldown.get(port) > Date.now() + 5000);
      }
      if (scenario === 'giveup') {
        assert.match(receiver.state.error, /重新给设备上电/);
      }
      if (scenario !== 'release-then-reset' && scenario !== 'giveup') {
        assert.doesNotMatch(receiver.state.error || '', /自动复位/);
      }
      await receiver.stop();
    });
}
