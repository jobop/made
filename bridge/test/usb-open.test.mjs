import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import childProcess from 'node:child_process';
import { syncBuiltinESMExports } from 'node:module';

for (const outcome of ['success', 'failure', 'stop']) {
  test(`USB 配置保持串口句柄，${outcome} 路径释放一次`,
    { skip: !['darwin', 'linux'].includes(process.platform), timeout: 3000 }, async (t) => {
      const port = '/dev/vibe-test-only';
      const fd = 1234567;
      const held = new Set();
      const closed = [];
      let configureCallback;
      const execFile = childProcess.execFile;
      const openSync = fs.openSync;
      const close = fs.close;
      // Model the WCH requirement: configuration only survives when the
      // bridge's own handle remains open during the stty subprocess.
      childProcess.execFile = (command, args, options, callback) => {
        assert.equal(command, 'stty');
        assert.deepEqual([...held], [fd]);
        assert.deepEqual(args.slice(1), [port, '921600', 'raw', '-echo', 'cs8',
          '-parenb', '-cstopb', '-crtscts', 'clocal', '-hupcl']);
        configureCallback = callback;
      };
      fs.openSync = (target, flags) => {
        if (target !== port) return openSync(target, flags);
        assert.ok(flags & fs.constants.O_NONBLOCK);
        if (fs.constants.O_NOCTTY) assert.ok(flags & fs.constants.O_NOCTTY);
        held.add(fd);
        return fd;
      };
      fs.close = (value, callback) => {
        if (value !== fd) return close(value, callback);
        assert.equal(held.delete(value), true, '句柄应且仅应关闭一次');
        closed.push(value);
        queueMicrotask(() => callback(null));
      };
      syncBuiltinESMExports();
      t.after(() => {
        childProcess.execFile = execFile;
        fs.openSync = openSync;
        fs.close = close;
        syncBuiltinESMExports();
      });
      const { UsbReceiver } = await import(`../src/usb-receiver.mjs?open-test=${outcome}`);
      const receiver = new UsbReceiver({ devicePort: 1, portPath: port });
      receiver.stopped = false;
      const pending = receiver.poll();
      assert.equal(typeof configureCallback, 'function');
      if (outcome === 'stop') await receiver.stop();
      configureCallback(outcome === 'failure' ? new Error('stty failed') : null, '', '');
      await pending;
      if (outcome === 'success') {
        assert.equal(receiver.link.fd, fd);
        assert.deepEqual([...held], [fd]);
        await receiver.stop();
      } else {
        assert.equal(receiver.links.size, 0);
        assert.equal(receiver.link, null);
      }
      assert.deepEqual(closed, [fd]);
      assert.equal(held.size, 0);
    });
}
