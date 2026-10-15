// CLI 执行工具箱：把 CLI 型助手插件需要的机制汇总成一份，经 config.runtime
// 注入插件。权限边界指令统一在 runCli 里拼（设备侧唯一护栏，不允许各插件
// 各写一遍）；平台差异（Windows .cmd、PATHEXT）统一在 plugin-runtime.mjs。
//
// 内置 CLI 助手（codex/cursor/qoder）自 2026-10 起已插件化为市场包，桥接器
// 不再内置任何编程助手；这套机制是它们在桥接器侧的公共底座。
import { probeCli, runCli, gitState } from './shared.mjs';
import {
  jsonLineEvents, assistantEvent, streamActivity, toolActivity, textBlocks,
} from './events.mjs';

export const cliToolbox = Object.freeze({
  probeCli, runCli, gitState,
  jsonLineEvents, assistantEvent, streamActivity, toolActivity, textBlocks,
});
