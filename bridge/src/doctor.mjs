/**
 * 桥接器自检：node src/server.mjs --doctor
 *
 * 为什么要有它：平台差异不该让用户去读代码。这里在真机上逐项检查哪些能力可用、
 * 缺什么、缺了怎么办，并把与平台相关的限制明说——与 AGENTS.md 的约定一致：
 * 功能不可用时必须给出原因，不能静默失败。模块化拆分是为了能直接测，
 * 不用把桥接器真的跑起来。
 */
import { ROOT, loadConfig } from './config.mjs';
import { findCommand, isWindows, platform } from './plugin-runtime.mjs';
import { discoverUsbSerialPorts } from './usb-receiver.mjs';
import { loadConfiguredPlugins } from './plugins/registry.mjs';
import { providerStates } from './providers.mjs';

const LEVEL = { ok: '✓', warn: '!', fail: '✗' };
const PLATFORM_LABEL = { darwin: 'macOS', linux: 'Linux', win32: 'Windows' };
const TUNNEL_TOOLS = ['tailscale', 'ngrok', 'cloudflared'];

const platformLabel = () => `${PLATFORM_LABEL[platform] || platform}（${platform}）`;

/** 串口发现失败不该让自检整体失败：拿不到就当没发现。 */
async function safePorts() {
  try {
    return await discoverUsbSerialPorts();
  } catch {
    return [];
  }
}

/**
 * 收集自检项。返回 { checks, code }：
 *   checks —— { level, label, detail } 或 { raw } 的纯数据，便于测试与二次渲染
 *   code   —— 进程退出码，1 表示有阻断性缺项
 */
export async function collectDoctor({ config: provided } = {}) {
  const checks = [];
  const add = (level, label, detail = '') => checks.push({ level, label, detail });
  const raw = (text = '') => checks.push({ raw: text });

  raw('码得桥接器自检');
  raw();

  raw('运行环境');
  add('ok', `${platformLabel()} · Node ${process.version}`);
  add('ok', `桥接器目录 ${ROOT}`);

  let config = provided;
  if (!config) {
    try {
      config = loadConfig();
    } catch (error) {
      add('fail', '配置无法加载', error.message);
    }
  }
  if (config) {
    add('ok', `配置文件 ${config.configFile}`);
    add('ok', `包目录 ${config.madeHome}（plugins/、themes/）`);
    add('ok', `状态目录 ${config.stateDir}`);
    add('ok', `端口：电脑面板 ${config.port}，设备接口 ${config.devicePort}`);
  }

  raw();
  raw('依赖命令');
  const git = findCommand('git');
  if (git) add('ok', `git ${git}`, '任务前后改动对比依赖它');
  else add('fail', '未找到 git', '任务的前后改动对比依赖它，请先安装 Git 并加入 PATH');

  const npm = findCommand('npm');
  if (npm) add('ok', `npm ${npm}`, '需要自装依赖的插件（如 Pi 智能体）首次运行时用它');
  else add('warn', '未找到 npm', '安装 Node.js 时自带 npm；缺了它，首次运行需要自动装依赖的插件会起不来');

  const esptool = findCommand('esptool') || findCommand('esptool.py');
  if (esptool) add('ok', `esptool ${esptool}`, '只给刷机工具用，桥接器运行不需要');
  else add('warn', '未找到 esptool', '只影响 npm run flash 刷机，不影响桥接器运行');

  raw();
  raw('接入方式');
  const ports = config ? await safePorts() : [];
  if (isWindows) {
    // Windows 侧没有 /dev，也没有 VID/PID 枚举，实话实说。
    add('warn', 'USB 线直连 / 接收器转发：不能自动发现串口',
      '需手动设 VIBE_USB_PORT=<COM 口>（设备管理器里可查）；原生 USB 变体可用，'
      + 'WCH 转串口变体因为不设波特率大概率不通');
  } else if (ports.length) {
    add('ok', `USB 线直连 / 接收器转发：已发现串口 ${ports.join('、')}`);
  } else {
    add('warn', 'USB 线直连 / 接收器转发：没有发现串口',
      '插上码得或接收器后重试；也可以直接用 VIBE_USB_PORT 指定端口');
  }
  if (config?.usbPort) add('ok', `已用 VIBE_USB_PORT 指定 ${config.usbPort}`);

  add('ok', '局域网 Wi-Fi 直连：设备与电脑同网即可，无需额外配置');
  const tunnels = TUNNEL_TOOLS.filter((name) => findCommand(name));
  if (tunnels.length) add('ok', `公网地址：本机有 ${tunnels.join('、')}`);
  else add('warn', '公网地址：未检测到内网穿透工具',
    `装了 ${TUNNEL_TOOLS.join(' / ')} 之一才能在桥接页配置公网地址`);

  if (esptool) {
    add('ok', '烧录：npm run flash 可用（先停掉占用串口的桥接器）');
  } else {
    add('warn', '烧录：npm run flash 需要 esptool，当前不可用');
  }

  if (config) {
    raw();
    raw('插件');
    try {
      const registry = await loadConfiguredPlugins(config);
      const states = providerStates({ ...config, pluginsRuntime: registry });
      if (!states.length) add('warn', '没有加载到任何助手插件', '插件目录见上方的包目录');
      for (const state of states) {
        add(state.available ? 'ok' : 'warn', `${state.label}（${state.id}）`, state.reason || '可用');
      }
    } catch (error) {
      add('fail', '插件加载失败', error.message);
    }
  }

  const fails = checks.filter((item) => item.level === 'fail').length;
  const warns = checks.filter((item) => item.level === 'warn').length;
  raw();
  raw(fails
    ? `自检未通过：${fails} 项需要处理${warns ? `，另有 ${warns} 项受限` : ''}。详见上方 ✗ 项。`
    : warns
      ? `自检通过：${warns} 项受限（上方 ! 项），不影响其余功能。`
      : '自检通过：本机没有发现问题。');

  return { checks, code: fails ? 1 : 0 };
}

/** 把自检项渲染成给人看的文本。 */
export function formatDoctor(checks) {
  return checks
    .map((item) => (item.raw !== undefined
      ? item.raw
      : `${LEVEL[item.level]} ${item.label}${item.detail ? `\n    ${item.detail}` : ''}`))
    .join('\n');
}

/** 命令行入口：打印自检结果并返回进程退出码。 */
export async function runDoctor(options = {}) {
  const { out = console.log, ...rest } = options;
  const { checks, code } = await collectDoctor(rest);
  out(formatDoctor(checks));
  return code;
}
