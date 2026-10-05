import fs from 'node:fs';
import { translateKnownMessage } from '../i18n.mjs';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';

const authenticationCache = new Map();

export function probeCli(command, loginArgs) {
  let found;
  for (const directory of (process.env.PATH || '').split(path.delimiter)) {
    const candidate = path.join(directory || '.', command);
    try {
      fs.accessSync(candidate, fs.constants.X_OK);
      if (fs.statSync(candidate).isFile()) { found = candidate; break; }
    } catch { /* Try the next PATH entry. */ }
  }
  let authenticated = !loginArgs;
  if (found && loginArgs) {
    const key = `${found}:${loginArgs.join(' ')}`;
    const cached = authenticationCache.get(key);
    if (cached && cached.until > Date.now()) authenticated = cached.value;
    else {
      const check = spawnSync(found, loginArgs, { encoding: 'utf8', timeout: 3000 });
      authenticated = check.status === 0 && !/not logged in|not authenticated|login required/i.test(`${check.stdout || ''} ${check.stderr || ''}`);
      authenticationCache.set(key, { value: authenticated, until: Date.now() + 60000 });
    }
  }
  const available = Boolean(found && authenticated);
  return { available, mode: 'local_cli', reason: available ? '' : found
    ? `请先在本机登录 ${command}` : `本机未找到 ${command} 命令` };
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
  return { dirty: status.stdout.length > 0, snapshot: hash.digest('hex') };
}

function repositorySnapshot(cwd) {
  if (!fs.existsSync(cwd)) {
    fs.mkdirSync(cwd, { recursive: true });
    spawnSync('git', ['init'], { cwd, encoding: 'utf8', timeout: 15000 });
  }
  if (!fs.statSync(cwd).isDirectory()) {
    throw new Error(`代码目录不存在：${cwd}`);
  }
  const repo = spawnSync('git', ['rev-parse', '--show-toplevel'], {
    cwd, encoding: 'utf8', timeout: 5000,
  });
  if (repo.status !== 0) {
    throw new Error('目标项目必须是 Git 仓库，以便查看和恢复改动');
  }
  const root = repo.stdout.trim();
  if (path.resolve(root) !== path.resolve(cwd)) {
    throw new Error('请把项目路径设为 Git 仓库根目录');
  }
  return gitState(cwd).snapshot;
}

function gitChangeSummary(cwd) {
  const result = spawnSync('git', ['status', '--short'], {
    cwd, encoding: 'utf8', timeout: 5000,
  });
  return result.status === 0 ? result.stdout.trim().slice(0, 4000) : '';
}

export function runCli({ job, project, signal, onProgress, onSession, config, model }, adapter) {
  const startingGitSnapshot = repositorySnapshot(project.path);
  const message = (value) => translateKnownMessage(value, config.locale);
  const prompt = job.instruction;
  const spec = adapter.command({ cwd: project.path, instruction: prompt, config, model, sessionId: job.sessionId });
  return new Promise((resolve, reject) => {
    const events = adapter.createEvents(onProgress, onSession);
    const child = spawn(spec.command, spec.args, {
      cwd: project.path,
      stdio: ['pipe', 'pipe', 'pipe'],
      detached: process.platform !== 'win32',
      env: process.env,
    });
    let stderr = '';
    let settled = false;
    let timer;
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
      signal.removeEventListener('abort', stop);
      if (error) reject(error); else resolve(value);
    };
    signal.addEventListener('abort', stop, { once: true });
    if (signal.aborted) stop();
    timer = setTimeout(() => {
      stop();
      finish(new Error('任务运行超过 20 分钟，已停止'));
    }, 20 * 60 * 1000);
    child.on('error', (error) => finish(error));
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', (chunk) => events.write(chunk));
    child.stderr.on('data', (chunk) => {
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
      finish(null, {
        status: 'completed',
        result: String(summary).slice(-12000),
        gitSnapshot: gitState(project.path).snapshot,
      });
      } catch (error) { finish(error); }
    });
    child.stdin.end(spec.stdin || '');
  });
}
