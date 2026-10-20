import fs from 'node:fs';
import { translateKnownMessage } from '../i18n.mjs';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { findCommand, isWindows, windowsCommandLine } from '../plugin-runtime.mjs';

const authenticationCache = new Map();

/**
 * 每轮任务都要带上的权限边界。CLI 助手拥有桥接器进程的权限，这条约束是设备侧
 * 唯一的护栏，不能只在某几个适配器里各写一遍，所以统一在 runCli 里拼。
 */
const GUARD_INSTRUCTION = '请只修改当前仓库中的文件；不要提交、推送、发布或删除仓库。完成后说明改动与已运行的验证。';

export function probeCli(command, loginArgs) {
  // 命令查找走共享的跨平台实现：Windows 上要按 PATHEXT 补 .cmd/.exe，
  // 且 X_OK 在那里没有意义，自己写一遍最容易漏。
  const found = findCommand(command);
  let authenticated = !loginArgs;
  if (found && loginArgs) {
    const key = `${found}:${loginArgs.join(' ')}`;
    const cached = authenticationCache.get(key);
    if (cached && cached.until > Date.now()) authenticated = cached.value;
    else {
      const check = spawnCommandSync(found, loginArgs, { encoding: 'utf8', timeout: 3000 });
      authenticated = check.status === 0 && !/not logged in|not authenticated|login required/i.test(`${check.stdout || ''} ${check.stderr || ''}`);
      authenticationCache.set(key, { value: authenticated, until: Date.now() + 60000 });
    }
  }
  const available = Boolean(found && authenticated);
  return { available, mode: 'local_cli', reason: available ? '' : found
    ? `请先在本机登录 ${command}` : `本机未找到 ${command} 命令` };
}

/** 同步启动一个已解析到绝对路径的命令：.cmd/.bat 经 cmd.exe，其余直连。 */
function spawnCommandSync(resolved, args, options) {
  return isWindows && /\.(?:cmd|bat)$/i.test(resolved)
    ? spawnSync(windowsCommandLine([resolved, ...args]), { ...options, shell: true })
    : spawnSync(resolved, args, options);
}

export function gitState(cwd) {
  const hasHead = spawnSync('git', ['rev-parse', '--verify', 'HEAD'], {
    cwd, stdio: 'ignore', timeout: 5000,
  }).status === 0;
  const status = spawnSync('git', ['status', '--porcelain', '-z'], {
    cwd, timeout: 5000, maxBuffer: 32 * 1024 * 1024,
  });
  const diff = spawnSync('git', hasHead
    ? ['diff', '--no-ext-diff', '--binary', 'HEAD']
    : ['diff', '--no-ext-diff', '--binary'], {
    cwd, timeout: 5000, maxBuffer: 32 * 1024 * 1024,
  });
  const staged = hasHead ? null : spawnSync('git', ['diff', '--no-ext-diff', '--binary', '--cached'], {
    cwd, timeout: 5000, maxBuffer: 32 * 1024 * 1024,
  });
  const untracked = spawnSync('git', ['ls-files', '--others', '--exclude-standard', '-z'], {
    cwd, timeout: 5000, maxBuffer: 32 * 1024 * 1024,
  });
  if (status.status !== 0 || diff.status !== 0 || untracked.status !== 0 ||
      (staged && staged.status !== 0)) {
    throw new Error('无法检查项目改动状态');
  }
  const hash = createHash('sha256').update(status.stdout).update(diff.stdout);
  if (staged) hash.update(staged.stdout);
  for (const relative of untracked.stdout.toString('utf8').split('\0').filter(Boolean)) {
    const filename = path.join(cwd, relative);
    const stat = fs.lstatSync(filename);
    hash.update(relative);
    if (stat.isFile() && stat.size <= 4 * 1024 * 1024) hash.update(fs.readFileSync(filename));
    else hash.update(`${stat.size}:${stat.mtimeMs}`);
  }
  return {
    dirty: status.stdout.length > 0,
    snapshot: hash.digest('hex'),
    // 改动清单直接从上面这次 `git status --porcelain -z` 里取，收尾时不必再跑一次 git：
    // 少一个子进程，也少一分超时风险。porcelain 的条目本身就是「 XY 路径」这种可读形状。
    changed: status.stdout.toString('utf8').split('\0').filter(Boolean).slice(0, 40).join('\n'),
  };
}

function repositorySnapshot(cwd) {
  if (!fs.existsSync(cwd)) fs.mkdirSync(cwd, { recursive: true });
  if (!fs.statSync(cwd).isDirectory()) {
    throw new Error(`代码目录不存在：${cwd}`);
  }
  let repo = spawnSync('git', ['rev-parse', '--show-toplevel'], {
    cwd, encoding: 'utf8', timeout: 5000,
  });
  if (repo.status !== 0) {
    spawnSync('git', ['init'], { cwd, encoding: 'utf8', timeout: 15000 });
    repo = spawnSync('git', ['rev-parse', '--show-toplevel'], {
      cwd, encoding: 'utf8', timeout: 5000,
    });
  }
  if (repo.status !== 0) {
    throw new Error('目标项目必须是 Git 仓库，以便查看和恢复改动');
  }
  const root = repo.stdout.trim();
  if (path.resolve(root) !== path.resolve(cwd)) {
    throw new Error('请把项目路径设为 Git 仓库根目录');
  }
  return gitState(cwd).snapshot;
}

/** 收尾附上本轮改了什么：有改动就列出文件，没有就明说，别让用户去猜。 */
function changeFooter(state, message) {
  return state.changed ? `${message('改动文件：')}\n${state.changed}` : message('没有检测到代码改动。');
}

export function runCli({ job, project, signal, onProgress, onSession, onTool, config, model }, adapter) {
  const startingGitSnapshot = repositorySnapshot(project.path);
  const message = (value) => translateKnownMessage(value, config.locale);
  // 每轮都拼上权限边界：用户原文一字不改，边界文案按界面语言翻译。
  const prompt = `${job.instruction}\n\n${message(GUARD_INSTRUCTION)}`;
  const spec = adapter.command({ cwd: project.path, instruction: prompt, config, model, sessionId: job.sessionId });
  return new Promise((resolve, reject) => {
    const events = adapter.createEvents(onProgress, onSession);
    // 命令先解析成绝对路径：Windows 上 codex/cursor 这类 CLI 实际是 .cmd，
    // 直接 spawn 会 EINVAL；经 cmd.exe 启动，并自己把参数引号拼好。
    const resolved = findCommand(spec.command) || spec.command;
    const viaCmd = isWindows && /\.(?:cmd|bat)$/i.test(resolved);
    const child = viaCmd
      ? spawn(process.env.ComSpec || 'cmd.exe', ['/d', '/s', '/c', `"${windowsCommandLine([resolved, ...spec.args])}"`], {
        cwd: project.path,
        stdio: ['pipe', 'pipe', 'pipe'],
        windowsVerbatimArguments: true,
        env: process.env,
      })
      : spawn(resolved, spec.args, {
        cwd: project.path,
        stdio: ['pipe', 'pipe', 'pipe'],
        detached: process.platform !== 'win32',
        env: process.env,
      });
    let stderr = '';
    let settled = false;
    let timer;
    // 卡死看门狗：编程工具进程连续 idleMs 没有任何输出（进度、流式、stderr 都算）
    // 视为卡死，杀掉进程树并把错误标记为可自动重跑，由任务车道决定是否重试。
    const idleMs = Number(config?.stuckIdleMs)
      || Math.max(1, Number(config?.stuckIdleMinutes) || 5) * 60 * 1000;
    let idleTimer;
    const bumpIdle = () => {
      clearTimeout(idleTimer);
      idleTimer = setTimeout(() => {
        stop();
        const error = new Error(`任务 ${Math.round(idleMs / 60000)} 分钟没有任何输出，疑似卡死：已终止编程工具进程`);
        error.retryable = true;
        error.autoRetry = true;
        finish(error);
      }, idleMs);
      if (idleTimer.unref) idleTimer.unref();
    };
    const stop = () => {
      if (child.pid && process.platform !== 'win32') {
        try { process.kill(-child.pid, 'SIGTERM'); } catch { /* Already exited. */ }
        const forceTimer = setTimeout(() => {
          if (child.exitCode === null) {
            try { process.kill(-child.pid, 'SIGKILL'); } catch { /* Already exited. */ }
          }
        }, 3000);
        forceTimer.unref();
      } else {
        child.kill('SIGTERM');
      }
    };
    const finish = (error, value) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      clearTimeout(idleTimer);
      signal.removeEventListener('abort', stop);
      if (error) reject(error); else resolve(value);
    };
    signal.addEventListener('abort', stop, { once: true });
    if (signal.aborted) stop();
    bumpIdle();
    timer = setTimeout(() => {
      stop();
      const error = new Error('任务运行超过 20 分钟，已停止');
      error.retryable = true;
      error.autoRetry = true;
      finish(error);
    }, 20 * 60 * 1000);
    child.on('error', (error) => finish(error));
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', (chunk) => {
      bumpIdle();
      events.write(chunk);
      // 工具调用只走 onTool（桥接器任务记录，网页端折叠展示），不进设备进度文本。
      const forwarded = events.takeToolCalls?.() || [];
      for (const call of forwarded) onTool?.(call);
    });
    child.stderr.on('data', (chunk) => {
      bumpIdle();
      stderr = `${stderr}${chunk.toString('utf8')}`.slice(-8000);
    });
    child.on('close', (code) => {
      if (settled) return;
      try {
      if (signal.aborted) return finish(new Error('任务已取消'));
      const output = events.finish();
      if (code !== 0) {
        const classified = adapter.classifyExit?.({ job, project, stderr, output, startingGitSnapshot });
        if (classified) return finish(classified);
        return finish(new Error(`${adapter.label} 退出码 ${code}：${stderr.trim().slice(-300) || message(output.error || '请在电脑端检查 CLI 登录和权限设置')}`));
      }
      if (output.error) return finish(new Error(`${adapter.label}：${output.error}`));
      const summary = output.hasAssistantText === false ? message(output.result) : output.result;
      const state = gitState(project.path);
      finish(null, {
        status: 'completed',
        result: `${String(summary).slice(-12000)}\n\n${changeFooter(state, message)}`,
        gitSnapshot: state.snapshot,
      });
      } catch (error) { finish(error); }
    });
    child.stdin.end(spec.stdin || '');
  });
}
