/**
 * 插件运行时助手：跨平台的命令查找与子进程调用。
 *
 * 为什么单独放一个文件：插件常要包一层第三方 CLI 或 npm 包，而平台差异全在细节里——
 *   - Windows 上 npm 实际是 npm.cmd，Node 20 起不允许直接 spawn .cmd/.bat（报 EINVAL），
 *     必须经 cmd.exe；而 Node 在 shell 模式下只是把命令和参数用空格拼起来，含空格的
 *     路径（C:\Users\John Doe\…）会被 cmd 拆成两个参数，引号得自己加。
 *   - PATH 探测在 Windows 上要按 PATHEXT 逐个补扩展名，且 X_OK 在 Windows 上没有意义。
 *   - 「命令不存在」和「命令存在但执行失败」是两件事，报错必须分开，否则排查会被带偏。
 * 这些收在一处，插件作者不必各自踩一遍，也不用引入 cross-spawn 之类的依赖。
 *
 * 插件通过 `config.runtime` 拿到这些函数，例如：
 *   const { runNpm } = config.runtime;
 *   const globalRoot = runNpm(['root', '-g']).trim();
 */
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

export const platform = process.platform;
export const isWindows = platform === 'win32';

/** 命令是否已自带扩展名，Windows 上带了就不用再补 PATHEXT。 */
const HAS_EXTENSION = /\.(?:exe|com|bat|cmd|ps1)$/i;

/** Windows 下的可执行扩展名，按 PATHEXT 取，缺省给一套常见值。 */
function windowsExtensions(env) {
  const raw = env.PATHEXT || '.COM;.EXE;.BAT;.CMD';
  return raw.split(';').map((s) => s.trim()).filter(Boolean);
}

/**
 * 单个文件能否当命令执行。Windows 上没有可执行位，能读到即可；
 * 类 Unix 按 X_OK 判，避免把普通数据文件当成命令。
 */
function isExecutable(filename) {
  try {
    if (!fs.statSync(filename).isFile()) return false;
    fs.accessSync(filename, isWindows ? fs.constants.F_OK : fs.constants.X_OK);
    return true;
  } catch {
    return false;
  }
}

/**
 * 在 PATH 里查一个命令，返回绝对路径；找不到返回空串。
 * 传了路径分隔符的命令按字面路径处理，不再搜 PATH。
 */
export function findCommand(command, { env = process.env } = {}) {
  const name = String(command || '').trim();
  if (!name) return '';
  if (name.includes('/') || name.includes('\\')) {
    return isExecutable(name) ? path.resolve(name) : '';
  }
  const suffixes = isWindows && !HAS_EXTENSION.test(name) ? ['', ...windowsExtensions(env)] : [''];
  for (const directory of (env.PATH || '').split(path.delimiter)) {
    for (const suffix of suffixes) {
      const candidate = path.join(directory || '.', name + suffix);
      if (isExecutable(candidate)) return candidate;
    }
  }
  return '';
}

/** 这条命令在本机是否存在（跨平台，等价于「装了没有」）。 */
export function hasCommand(command, options) {
  return Boolean(findCommand(command, options));
}

/**
 * 按 cmd.exe 的规则给一个参数加引号。Node 在 shell 模式下不做这层处理，
 * 含空格或 cmd 元字符的参数必须自己包好，否则会被拆开或被当成控制字符。
 */
function quoteWindowsArgument(value) {
  const text = String(value ?? '');
  if (text === '') return '""';
  if (/[\s"^&|<>()]/.test(text)) return `"${text.replace(/"/g, '""')}"`;
  return text;
}

/** 把 argv 拼成一条 cmd.exe 命令行。启动 .cmd/.bat 时用它，异步 spawn 也用同一套。 */
export function windowsCommandLine(argv) {
  return argv.map(quoteWindowsArgument).join(' ');
}

/**
 * 同步执行一条命令。返回 spawnSync 的结果，并补两样东西：
 *   resolved —— 实际用的可执行文件路径；notFound —— 是不是「本机没这个命令」。
 * stdio 传 'inherit' 时不做编码转换，交给子进程直连终端。
 */
export function runCommand(command, args = [], {
  env = process.env, cwd, timeout, input, encoding = 'utf8', stdio,
} = {}) {
  const resolved = findCommand(command, { env });
  if (!resolved) {
    const error = Object.assign(new Error(`本机未找到 ${command} 命令`), { code: 'ENOENT' });
    return { status: null, stdout: '', stderr: '', error, resolved: '', notFound: true };
  }
  const options = { env, cwd, timeout };
  if (stdio) options.stdio = stdio;
  else {
    options.encoding = encoding;
    if (input !== undefined) options.input = input;
  }
  // .cmd/.bat 只能经 cmd.exe 启动；命令与参数在这里拼成一条命令行。
  const result = isWindows && /\.(?:cmd|bat)$/i.test(resolved)
    ? spawnSync(windowsCommandLine([resolved, ...args]), { ...options, shell: true })
    : spawnSync(resolved, args, options);
  return { ...result, resolved, notFound: false };
}

/**
 * 定位 npm。Windows 上 npm 是 npm.cmd，直接 spawn 会 EINVAL；能找到它旁边的
 * npm-cli.js 就改用 node 跑那个脚本，彻底绕开 cmd 的引号规则（更稳）。
 */
function locateNpm(env) {
  const command = findCommand('npm', { env });
  if (!command) return null;
  if (isWindows) {
    const script = path.join(path.dirname(command), 'node_modules', 'npm', 'bin', 'npm-cli.js');
    if (fs.existsSync(script)) return { command, script };
  }
  return { command, script: '' };
}

/** npm 执行失败时的错误：带上最后几行输出，「装了但跑不动」和「没装」在 message 里分得清。 */
function npmFailure(args, detail) {
  const tail = String(detail || '').trim().slice(-300);
  return Object.assign(
    new Error(`npm ${args.join(' ')} 执行失败${tail ? `：${tail}` : ''}`),
    { code: 'ENPMFAIL' },
  );
}

/**
 * 同步执行 npm，成功返回 stdout（原样，调用方自己 trim）。失败抛错：
 * 本机没装 npm 是 ENOENT（notFound=true），装了但执行失败是 ENPMFAIL。
 */
export function runNpm(args = [], { env = process.env, cwd, timeout, input } = {}) {
  const npm = locateNpm(env);
  if (!npm) {
    throw Object.assign(new Error('本机未找到 npm 命令，请先安装 Node.js（含 npm）'), {
      code: 'ENOENT', notFound: true,
    });
  }
  const options = { env, cwd, timeout, encoding: 'utf8' };
  if (input !== undefined) options.input = input;
  const result = npm.script
    ? spawnSync(process.execPath, [npm.script, ...args], options)
    : spawnSync(npm.command, args, options);
  if (result.error) {
    if (result.error.code === 'ENOENT') {
      throw Object.assign(new Error('本机未找到 npm 命令，请先安装 Node.js（含 npm）'), {
        code: 'ENOENT', notFound: true,
      });
    }
    throw result.error;
  }
  if (result.status !== 0) throw npmFailure(args, `${result.stderr || ''}${result.stdout || ''}`);
  return result.stdout || '';
}

/** 交给插件的那一份：冻结，避免插件改到别的插件或桥接器的行为。 */
export const pluginRuntime = Object.freeze({
  platform, isWindows, findCommand, hasCommand, runCommand, runNpm,
});

export default pluginRuntime;
