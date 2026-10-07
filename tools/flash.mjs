#!/usr/bin/env node
/**
 * 码得刷机工具
 *
 * 一条命令认板、选目标、烧录，覆盖四类目标：
 *   - 码得（方板 / 圆板 / 码得侧）→ made 固件，ESP32-S3 16MB
 *   - 接收器 · ESP32-S3              → 2MB，原生 USB
 *   - 接收器 · ESP32-C3              → 4MB
 *
 * 设计约束（与 AGENTS.md 一致）：
 *   - 认板再烧：先 esptool chip_id，用芯片与 PSRAM 容量判断是哪块板。
 *   - 默认不擦除 Flash，保住 NVS 里的 Wi-Fi、配对与接收端热点密码。
 *   - esptool 4.12 统一使用下划线参数（write_flash / default_reset / hard_reset）。
 *
 * 用法：
 *   npm run flash                        交互式：扫描 → 认板 → 推荐目标 → 确认 → 烧录
 *   npm run flash -- made                直接烧码得
 *   npm run flash -- receiver-c3         直接烧 C3 接收器
 *   npm run flash -- made --build        先构建再烧
 *   npm run flash -- --scan              只认板，不烧
 *   npm run flash -- --list              列出目标
 *   npm run flash -- made -p /dev/cu.usbmodem101 -y
 */

import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import net from 'node:net'
import crypto from 'node:crypto'
import readline from 'node:readline/promises'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')

// ── 目标定义 ────────────────────────────────────────────────────────────────
// source: 'flasher_args' 以构建目录里的 flasher_args.json 为地址表真源；
//         'fixed'        用下面列出的固定地址表（预构建 release 目录）。
const TARGETS = {
  made: {
    id: 'made',
    label: '码得（方板 / 圆板 / 码得侧）',
    aliases: ['made', '码得', '方板', '圆板', '码得侧', '方屏', '圆屏', 'round', 'square', 'host'],
    chip: 'esp32s3',
    flashSize: '16MB',
    imageDir: 'made/build-s3',
    source: 'flasher_args',
    // 认板时用于区分：码得是带 8MB PSRAM 的 S3
    expectPsramMB: 8,
    buildDir: 'made',
    buildArgs: ['-B', 'build-s3'],
  },
  'receiver-s3': {
    id: 'receiver-s3',
    label: '接收器 · ESP32-S3（原生 USB）',
    aliases: ['receiver', 'receiver-s3', '接收器', '接收器s3', '接收器-s3', 's3', 'rx-s3'],
    chip: 'esp32s3',
    flashSize: '2MB',
    imageDir: 'receiver/release',
    source: 'fixed',
    files: [
      { addr: '0x0', file: 'bootloader.bin' },
      { addr: '0x8000', file: 'partition-table.bin' },
      { addr: '0x10000', file: 'vibe_receiver.bin' },
    ],
    buildDir: 'receiver',
    buildArgs: ['-B', 'build-s3'],
  },
  'receiver-c3': {
    id: 'receiver-c3',
    label: '接收器 · ESP32-C3（UART / WCH 转串口）',
    aliases: ['receiver-c3', '接收器c3', '接收器-c3', 'c3', 'rx-c3', 'c3-uart'],
    chip: 'esp32c3',
    flashSize: '4MB',
    imageDir: 'receiver/release-esp32c3',
    source: 'fixed',
    files: [
      { addr: '0x0', file: 'bootloader.bin' },
      { addr: '0x8000', file: 'partition-table.bin' },
      { addr: '0x10000', file: 'vibe_receiver.bin' },
    ],
    buildDir: 'receiver',
    buildImageDir: 'receiver/build-c3-uart',
    buildArgs: [
      '-B', 'build-c3-uart',
      '-D', 'SDKCONFIG=sdkconfig.c3uart',
      '-D', 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32c3',
      '-D', 'IDF_TARGET=esp32c3',
    ],
  },
  'receiver-c3-usb': {
    id: 'receiver-c3-usb',
    label: '接收器 · ESP32-C3（原生 USB）',
    aliases: ['receiver-c3-usb', '接收器c3usb', '接收器-c3-usb', 'c3usb', 'c3-usb', 'rx-c3-usb'],
    chip: 'esp32c3',
    flashSize: '4MB',
    // 没有预构建 release，直接以构建目录为镜像来源
    imageDir: 'receiver/build-c3-usb',
    source: 'flasher_args',
    buildDir: 'receiver',
    buildImageDir: 'receiver/build-c3-usb',
    buildArgs: [
      '-B', 'build-c3-usb',
      '-D', 'SDKCONFIG=sdkconfig.c3usb',
      '-D', 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32c3;sdkconfig.defaults.esp32c3usb',
      '-D', 'IDF_TARGET=esp32c3',
    ],
  },
}

const BAUD = '460800'

// ── 小工具 ─────────────────────────────────────────────────────────────────
const useColor = process.stdout.isTTY && !process.env.NO_COLOR
const paint = (code) => (s) => (useColor ? `\x1b[${code}m${s}\x1b[0m` : String(s))
const bold = paint('1')
const dim = paint('2')
const red = paint('31')
const green = paint('32')
const yellow = paint('33')
const blue = paint('36')

const ok = (m) => console.log(`${green('✓')} ${m}`)
const warn = (m) => console.log(`${yellow('!')} ${m}`)
const fail = (m) => console.log(`${red('✗')} ${m}`)

const shq = (s) => `'${String(s).replace(/'/g, `'\\''`)}'`
const abs = (p) => path.resolve(ROOT, p)
const human = (n) => {
  if (n >= 1024 * 1024) return `${(n / 1024 / 1024).toFixed(2)} MB`
  if (n >= 1024) return `${(n / 1024).toFixed(1)} KB`
  return `${n} B`
}

function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { encoding: 'utf8', ...opts })
  return {
    code: r.status === null ? 1 : r.status,
    out: `${r.stdout || ''}${r.stderr || ''}`,
    raw: r,
  }
}

function die(msg) {
  fail(msg)
  process.exit(1)
}

// ── esptool 定位 ───────────────────────────────────────────────────────────
// 本机 esptool 在 ESP-IDF 的 python 环境里，且该环境是 py3.9（不是 py3.13）。
// 直接调用它的 python，避开 export.sh 的版本探测。
function findEsptool() {
  const candidates = []

  const idfEnv = path.join(os.homedir(), '.espressif', 'python_env')
  if (fs.existsSync(idfEnv)) {
    for (const name of fs.readdirSync(idfEnv).sort()) {
      const py = path.join(idfEnv, name, 'bin', 'python')
      if (fs.existsSync(py)) candidates.push({ cmd: py, prefix: ['-m', 'esptool'], from: name })
    }
  }

  for (const p of [
    path.join(os.homedir(), '.workbuddy', 'binaries', 'python', 'envs', 'default', 'bin', 'esptool'),
    '/opt/homebrew/bin/esptool',
    '/usr/local/bin/esptool',
  ]) {
    if (fs.existsSync(p)) candidates.push({ cmd: p, prefix: [], from: p })
  }

  const onPath = run('/usr/bin/which', ['esptool'])
  if (onPath.code === 0 && onPath.out.trim()) {
    candidates.push({ cmd: onPath.out.trim(), prefix: [], from: 'PATH' })
  }

  for (const c of candidates) {
    const probe = run(c.cmd, [...c.prefix, 'version'])
    const m = /esptool(?:\.py)?\s+v?(\d+\.\d+[.\d]*)/i.exec(probe.out)
    if (probe.code === 0 && m) return { ...c, version: m[1] }
  }
  return null
}

function esptoolRun(tool, args, { live = false } = {}) {
  if (live) {
    const r = spawnSync(tool.cmd, [...tool.prefix, ...args], { stdio: 'inherit' })
    return { code: r.status === null ? 1 : r.status }
  }
  return run(tool.cmd, [...tool.prefix, ...args])
}

// ── 串口扫描 ───────────────────────────────────────────────────────────────
function scanPorts() {
  const devDir = '/dev'
  if (!fs.existsSync(devDir)) return []
  let names = []
  try {
    names = fs.readdirSync(devDir)
  } catch {
    return []
  }
  const patterns =
    process.platform === 'darwin'
      ? [/^cu\.usbmodem/, /^cu\.usbserial/, /^cu\.wchusbserial/, /^cu\.SLAB_USBtoUART/]
      : [/^ttyUSB/, /^ttyACM/, /^ttyAMA/]

  return names
    .filter((n) => patterns.some((re) => re.test(n)))
    .map((n) => path.join(devDir, n))
    .sort()
}

// 谁占着这个串口（多半是桥接器）
function portHolder(port) {
  if (process.platform === 'darwin' || process.platform === 'linux') {
    const r = run('/usr/sbin/lsof', ['-F', 'pcu', port])
    if (r.code !== 0 && !r.out.trim()) {
      const r2 = run('/usr/bin/lsof', ['-F', 'pcu', port])
      if (r2.code === 0 || r2.out.trim()) return parseLsof(r2.out)
    }
    return parseLsof(r.out)
  }
  return null
}

function parseLsof(out) {
  const cmd = /^c(.+)$/m.exec(out)?.[1]
  const pid = /^p(\d+)$/m.exec(out)?.[1]
  return cmd ? { cmd, pid } : null
}

// ── 认板 ───────────────────────────────────────────────────────────────────
function probe(tool, port) {
  const info = { port, chip: null, chipRaw: null, psramMB: 0, flashSize: null, mac: null, nativeUsb: false }

  const chipId = esptoolRun(tool, ['--port', port, 'chip_id'])
  const text = chipId.out

  if (chipId.code !== 0) {
    const lower = text.toLowerCase()
    if (lower.includes('could not open port') || lower.includes('resource busy') || lower.includes('permission')) {
      const holder = portHolder(port)
      const who = holder ? `，当前被 ${holder.cmd}${holder.pid ? ` (pid ${holder.pid})` : ''} 占用` : ''
      throw new Error(`串口 ${port} 打不开${who}。若桥接器在运行，先停掉它再刷。`)
    }
    if (lower.includes('no serial data received') || lower.includes('failed to connect')) {
      throw new Error(`串口 ${port} 没有响应。按住 BOOT 再插一次 USB，然后重试。`)
    }
    throw new Error(`认板失败：\n${text.trim()}`)
  }

  info.chipRaw = /Chip is (ESP32-[A-Za-z0-9-]+)/.exec(text)?.[1] || null
  const psram = /Embedded PSRAM\s+(\d+)\s*MB/i.exec(text)
  if (psram) info.psramMB = Number(psram[1])
  info.mac = /MAC:\s*([0-9a-fA-F:]{11,})/.exec(text)?.[1]?.toLowerCase() || null
  // 原生 USB 会报 USB-Serial/JTAG；WCH 转串口的板子走 UART，没有这一行
  info.nativeUsb = /USB mode:\s*USB-Serial\/JTAG/i.test(text)

  if (info.chipRaw) {
    const c = info.chipRaw.toLowerCase()
    if (c.includes('esp32-s3')) info.chip = 'esp32s3'
    else if (c.includes('esp32-c3')) info.chip = 'esp32c3'
    else info.chip = c.replace(/[^a-z0-9]/g, '')
  }

  const flashId = esptoolRun(tool, ['--port', port, 'flash_id'])
  const fs2 = /Detected flash size:\s*(\S+)/i.exec(flashId.out)
  if (fs2) info.flashSize = fs2[1].toUpperCase()

  return info
}

function recommendTarget(info) {
  if (info.chip === 'esp32c3') return info.nativeUsb ? 'receiver-c3-usb' : 'receiver-c3'
  if (info.chip === 'esp32s3') return info.psramMB >= 8 ? 'made' : 'receiver-s3'
  return null
}

// ── 目标解析 / 校验 ────────────────────────────────────────────────────────
function resolveTarget(name) {
  if (!name) return null
  const key = String(name).trim().toLowerCase()
  if (TARGETS[key]) return TARGETS[key]
  for (const t of Object.values(TARGETS)) {
    if (t.aliases.some((a) => a.toLowerCase() === key)) return t
  }
  return null
}

function checkChipMatch(target, info, { force }) {
  const want = target.chip
  const got = info.chip
  if (!got) {
    warn(`没能识别芯片型号，跳过芯片校验。`)
    return true
  }
  if (want === got) {
    if (target.expectPsramMB && info.psramMB && info.psramMB < target.expectPsramMB) {
      warn(`码得通常带 ${target.expectPsramMB}MB PSRAM，当前只读到 ${info.psramMB}MB——可能不是码得主机。`)
    }
    return true
  }
  fail(`芯片不匹配：目标是「${target.label}」需要 ${want}，当前板子是 ${got}。`)
  console.log(dim(`   接收器固件绝不能刷到码得主机上，反之亦然。确认要跨型号烧录请加 --force。`))
  if (!force) {
    console.log(`   要刷成接收器？用 ${bold('npm run flash -- receiver-c3')} / ${bold('... receiver-s3')}`)
    process.exit(1)
  }
  warn('已加 --force，继续烧录（风险自负）。')
  return true
}

// ── 烧录计划 ───────────────────────────────────────────────────────────────
function buildPlan(target) {
  const dir = abs(target.imageDir)
  if (!fs.existsSync(dir)) {
    die(`找不到镜像目录 ${target.imageDir}。先构建：npm run flash -- ${target.id} --build`)
  }

  let flashMode = 'dio'
  let flashFreq = '80m'
  let flashSize = target.flashSize
  let entries = []

  if (target.source === 'flasher_args') {
    const fa = path.join(dir, 'flasher_args.json')
    if (!fs.existsSync(fa)) die(`缺少 ${target.imageDir}/flasher_args.json，先构建一次。`)
    const j = JSON.parse(fs.readFileSync(fa, 'utf8'))
    flashMode = j.flash_settings?.flash_mode || flashMode
    flashSize = j.flash_settings?.flash_size || flashSize
    flashFreq = j.flash_settings?.flash_freq || flashFreq
    entries = Object.entries(j.flash_files || {}).map(([addr, file]) => ({ addr, file }))
  } else {
    entries = target.files.map((f) => ({ ...f }))
  }

  const planned = []
  for (const e of entries) {
    const p = path.join(dir, e.file)
    if (!fs.existsSync(p)) die(`缺少镜像文件 ${target.imageDir}/${e.file}。先构建或补齐发布文件。`)
    planned.push({ addr: e.addr, file: e.file, abs: p, size: fs.statSync(p).size })
  }

  // 地址升序，便于人读
  planned.sort((a, b) => parseInt(a.addr, 16) - parseInt(b.addr, 16))

  const total = planned.reduce((s, p) => s + p.size, 0)
  return { dir, entries: planned, flashMode, flashSize, flashFreq, total }
}

function printPlan(target, info, plan) {
  console.log()
  console.log(bold(`  目标  `) + target.label)
  console.log(bold(`  串口  `) + `${info.port}  ${dim(`芯片 ${info.chipRaw || info.chip || '?'}${info.psramMB ? ` · PSRAM ${info.psramMB}MB` : ''}${info.flashSize ? ` · Flash ${info.flashSize}` : ''}${info.mac ? ` · ${info.mac}` : ''}`)}`)
  console.log(bold(`  镜像  `) + `${target.imageDir}  ${dim(`(${plan.flashMode}/${plan.flashFreq}/${plan.flashSize})`)}`)
  console.log()
  for (const e of plan.entries) {
    console.log(`    ${blue(e.addr.padEnd(10))} ${e.file.padEnd(30)} ${dim(human(e.size).padStart(10))}`)
  }
  console.log(`    ${dim(`${''.padEnd(10)} ${String(plan.entries.length + ' 个文件').padEnd(30)} ${human(plan.total).padStart(10)}`)}`)
  console.log()
  console.log(dim(`  不擦除 Flash，NVS（Wi-Fi / 配对 / 接收端热点密码）保留。`))
}

// ── 构建 ───────────────────────────────────────────────────────────────────
function buildFirmware(target) {
  const cmds = []
  cmds.push('export PATH="/usr/bin:$PATH"')
  cmds.push('# WorkBuddy 的 python shim 会让 idf.py 的 os.mkdir(build/log) 抛 PermissionError(EEXIST)')
  cmds.push('unset PYTHONPATH')
  cmds.push('source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1')
  cmds.push(`cd ${shq(abs(target.buildDir))}`)
  cmds.push(`idf.py ${target.buildArgs.map(shq).join(' ')} build`)
  const script = cmds.join('\n')

  console.log(dim(`  $ cd ${target.buildDir} && idf.py ${target.buildArgs.join(' ')} build`))
  const r = spawnSync('/bin/zsh', ['-c', script], { stdio: 'inherit' })
  if (r.status !== 0) die(`构建失败（退出码 ${r.status}）。`)
  ok('构建完成')
}

// ── SHA256 校验（release 目录带 SHA256SUMS 时） ─────────────────────────────
function verifySums(target, plan) {
  const sums = path.join(abs(target.imageDir), 'SHA256SUMS')
  if (!fs.existsSync(sums)) return
  const lines = fs.readFileSync(sums, 'utf8').split('\n').map((l) => l.trim()).filter(Boolean)
  let checked = 0
  for (const line of lines) {
    const m = /^([0-9a-f]{64})\s+\*?(.+)$/i.exec(line)
    if (!m) continue
    const [, want, file] = m
    const p = path.join(abs(target.imageDir), path.basename(file))
    if (!fs.existsSync(p)) continue
    const got = crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex')
    if (got.toLowerCase() !== want.toLowerCase()) die(`SHA256 不匹配：${file}`)
    checked++
  }
  if (checked) ok(`SHA256 校验通过（${checked} 个文件）`)
}

// ── 烧录 ───────────────────────────────────────────────────────────────────
function flashFirmware(tool, target, info, plan, { erase, dryRun }) {
  const args = [
    '--chip', target.chip,
    '--port', info.port,
    '-b', BAUD,
    '--before', 'default_reset',
    '--after', 'hard_reset',
  ]

  if (erase) {
    console.log(dim(`  $ esptool ${args.join(' ')} erase_flash`))
    if (!dryRun) {
      const r = esptoolRun(tool, [...args, 'erase_flash'], { live: true })
      if (r.code !== 0) die('擦除失败。')
    }
  }

  const writeArgs = [
    ...args,
    'write_flash',
    '--flash_mode', plan.flashMode,
    '--flash_size', plan.flashSize,
    '--flash_freq', plan.flashFreq,
  ]
  for (const e of plan.entries) writeArgs.push(e.addr, e.abs)

  console.log(dim(`  $ esptool … write_flash ${plan.entries.map((e) => `${e.addr} ${e.file}`).join(' ')}`))
  console.log()

  if (dryRun) {
    ok('dry-run：未实际写入。')
    return true
  }

  const r = esptoolRun(tool, writeArgs, { live: true })
  if (r.code !== 0) die('烧录失败。')

  console.log()
  ok(`烧录完成：${target.label} → ${info.port}`)
  return true
}

// ── 交互辅助 ───────────────────────────────────────────────────────────────
async function pickFromList(ask, title, items, render) {
  console.log(`\n${title}`)
  items.forEach((it, i) => console.log(`  ${bold(String(i + 1))}. ${render(it)}`))
  const ans = (await ask(`\n选择 [1-${items.length}]：`)).trim()
  const n = Number(ans)
  if (!Number.isInteger(n) || n < 1 || n > items.length) return null
  return items[n - 1]
}

// 逐个串口认板，不烧录
function scanAll(tool, ports) {
  console.log(dim(`\nesptool ${tool.version}（${tool.from}）`))
  console.log()
  for (const p of ports) {
    const h = portHolder(p)
    if (h) {
      console.log(`${yellow('!')} ${p}`)
      console.log(dim(`    被 ${h.cmd}${h.pid ? ` (pid ${h.pid})` : ''} 占用，无法认板；烧录前先停掉它。`))
      console.log()
      continue
    }
    process.stdout.write(dim(`  ${p}  认板中…`))
    try {
      const info = probe(tool, p)
      const rec = recommendTarget(info)
      const desc = `${info.chipRaw || info.chip || '未知芯片'}${info.psramMB ? ` · PSRAM ${info.psramMB}MB` : ''}${info.flashSize ? ` · Flash ${info.flashSize}` : ''}${info.mac ? ` · ${info.mac}` : ''}`
      process.stdout.write('\r' + ' '.repeat(72) + '\r')
      console.log(`${green('✓')} ${p}`)
      console.log(`    ${desc}`)
      if (rec) console.log(`    ${dim('推荐：')}${TARGETS[rec].label}   ${dim(`npm run flash -- ${rec} -p ${p}`)}`)
    } catch (e) {
      process.stdout.write('\r' + ' '.repeat(72) + '\r')
      console.log(`${red('✗')} ${p}`)
      console.log(`    ${String(e.message).split('\n')[0]}`)
    }
    console.log()
  }
}

// ── 帮助 ───────────────────────────────────────────────────────────────────
function printHelp() {
  console.log(`
${bold('码得刷机工具')}

${bold('用法')}
  npm run flash [目标] [选项]

${bold('目标')}`)
  for (const t of Object.values(TARGETS)) {
    console.log(`  ${t.id.padEnd(14)} ${t.label}  ${dim(`(${t.chip}, ${t.flashSize})`)}`)
  }
  console.log(`
  不带目标运行时进入交互模式：扫描串口、认板、按芯片推荐目标。

${bold('选项')}
  -p, --port <设备>   指定串口，默认自动扫描
      --build         先 idf.py build 再烧
      --scan          只扫描并认板，不烧录
      --list          列出目标
      --verify        烧前校验 release 目录的 SHA256SUMS
      --erase         整片擦除后再烧（会清 NVS，需二次确认）
  -y, --yes           跳过写入确认
      --dry-run       只显示将执行的命令，不实际写入
      --force         芯片型号不匹配时仍然继续（危险）
  -h, --help          本帮助
`)
}

// ── 主流程 ─────────────────────────────────────────────────────────────────
function parseArgs(argv) {
  const out = { target: null, port: null, build: false, scan: false, list: false, verify: false, erase: false, yes: false, dryRun: false, force: false, help: false }
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]
    switch (a) {
      case '-h': case '--help': out.help = true; break
      case '-p': case '--port': out.port = argv[++i]; break
      case '--build': out.build = true; break
      case '--scan': out.scan = true; break
      case '--list': out.list = true; break
      case '--verify': out.verify = true; break
      case '--erase': out.erase = true; break
      case '-y': case '--yes': out.yes = true; break
      case '--dry-run': out.dryRun = true; break
      case '--force': out.force = true; break
      default:
        if (a.startsWith('-')) die(`未知选项 ${a}，用 --help 查看用法。`)
        if (!out.target) out.target = a
        else die(`多余的参数 ${a}。`)
    }
  }
  return out
}

function printTargetList() {
  console.log(`\n${bold('可烧录目标')}\n`)
  for (const t of Object.values(TARGETS)) {
    console.log(`  ${bold(t.id)}`)
    console.log(`    ${t.label}`)
    console.log(`    ${dim(`芯片 ${t.chip} · Flash ${t.flashSize} · 镜像 ${t.imageDir}`)}`)
    console.log(`    ${dim(`别名：${t.aliases.join('、')}`)}`)
    console.log()
  }
}

async function main() {
  const opts = parseArgs(process.argv.slice(2))

  if (opts.help) return printHelp()
  if (opts.list) return printTargetList()

  const tool = findEsptool()
  if (!tool) die('找不到 esptool。请安装 ESP-IDF 工具链，或把 esptool 放进 PATH。')

  // 串口
  let ports = scanPorts()
  if (opts.port) {
    if (!fs.existsSync(opts.port)) die(`串口不存在：${opts.port}`)
    ports = [opts.port]
  }

  if (ports.length === 0) {
    die('没有找到串口。插上板子后重试（macOS 端口形如 /dev/cu.usbmodem*）。')
  }

  // 只认板
  if (opts.scan) return scanAll(tool, ports)

  const interactive = Boolean(process.stdin.isTTY)
  let rl = null
  const ask = async (q) => {
    if (!interactive) {
      die('当前不是交互终端：请把目标与参数一次给全，例如 npm run flash -- made -p <端口> -y。')
    }
    if (!rl) rl = readline.createInterface({ input: process.stdin, output: process.stdout })
    return rl.question(q)
  }

  try {
    console.log(dim(`\nesptool ${tool.version}（${tool.from}）`))

    // 选串口
    let port = ports[0]
    if (ports.length > 1) {
      if (!interactive) {
        fail('检测到多个串口：')
        for (const p of ports) {
          const h = portHolder(p)
          console.log(`  ${p}${h ? dim(`  ← ${h.cmd} 占用`) : ''}`)
        }
        die('请用 -p 指定要刷哪一个。')
      }
      const chosen = await pickFromList(ask, bold('检测到多个串口：'), ports, (p) => {
        const h = portHolder(p)
        return `${p}${h ? dim(`  ← ${h.cmd} 正在占用`) : ''}`
      })
      if (!chosen) die('未选择串口。')
      port = chosen
    } else {
      console.log(`串口：${port}`)
    }

    // 认板
    process.stdout.write(dim('认板中…'))
    let info
    try {
      info = probe(tool, port)
    } catch (e) {
      process.stdout.write('\r' + ' '.repeat(72) + '\r')
      die(e.message)
    }
    process.stdout.write('\r' + ' '.repeat(72) + '\r')
    ok(`认到 ${info.chipRaw || info.chip}${info.psramMB ? ` · PSRAM ${info.psramMB}MB` : ''}${info.flashSize ? ` · Flash ${info.flashSize}` : ''}${info.mac ? ` · ${info.mac}` : ''}`)

    const rec = recommendTarget(info)
    if (rec) console.log(dim(`  按芯片推荐目标：${TARGETS[rec].label}`))

    // 选目标
    let target = resolveTarget(opts.target)
    if (!target) {
      if (opts.target) die(`未知目标「${opts.target}」，用 --list 查看。`)
      const recTarget = rec ? TARGETS[rec] : null
      if (recTarget && opts.yes) {
        target = recTarget
      } else {
        if (!interactive) {
          die(`未指定目标。用 npm run flash -- <目标> -p ${port}，目标见 npm run flash -- --list。` +
            (recTarget ? `\n  按芯片推荐：${recTarget.id}` : ''))
        }
        const order = []
        if (recTarget) order.push(recTarget)
        for (const t of Object.values(TARGETS)) if (t !== recTarget) order.push(t)
        const chosen = await pickFromList(ask, bold('烧哪个？'), order, (t) =>
          `${t.label}${t === recTarget ? green('  ← 推荐') : ''}${t.chip !== info.chip ? yellow('  (芯片不符)') : ''}`)
        if (!chosen) die('未选择目标。')
        target = chosen
      }
    }

    checkChipMatch(target, info, opts)

    if (opts.build) {
      console.log()
      buildFirmware(target)
      // 构建后改从构建目录取镜像（地址表以该目录的 flasher_args.json 为准）
      if (target.buildImageDir && target.buildImageDir !== target.imageDir) {
        target = { ...target, imageDir: target.buildImageDir, source: 'flasher_args' }
      }
    }

    const plan = buildPlan(target)
    if (opts.verify) verifySums(target, plan)
    printPlan(target, info, plan)

    if (opts.erase) {
      console.log()
      warn('已选择整片擦除：NVS 里的 Wi-Fi、配对令牌、接收端热点密码都会丢失。')
      if (!opts.yes) {
        const a = (await ask(red('确认整片擦除？输入 erase 继续：'))).trim()
        if (a !== 'erase') { warn('已取消。'); return }
      }
    }

    if (!opts.yes && !opts.dryRun) {
      const a = (await ask(bold('开始烧录？[Y/n] '))).trim().toLowerCase()
      if (a && a !== 'y' && a !== 'yes') { warn('已取消。'); return }
    }

    console.log()
    flashFirmware(tool, target, info, plan, opts)

    console.log()
    console.log(dim('  提示：板子已硬复位。若桥接器在运行，它会自动重新握手；否则在仓库根执行 npm start。'))
    if (target.id === 'made') {
      console.log(dim('  码得锁屏不连桥接器、不上报心跳，上滑解锁后才会重新上线，大盘短暂显示离线是正常的。'))
    }
  } finally {
    if (rl) rl.close()
  }
}

main().catch((e) => die(e?.message || String(e)))
