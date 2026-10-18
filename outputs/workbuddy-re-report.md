# WorkBuddy 桌面应用逆向分析报告（REA + 人工深挖）

日期：2026-10-17 · 目标：本机 macOS 安装的 WorkBuddy 5.8.2 · 工具：rea-agents（morluto/rea）

## 1. 目标概况

| 项 | 值 |
|---|---|
| 应用 | /Applications/WorkBuddy.app（`@genie/workbuddy-desktop` v5.8.2） |
| 架构 | Electron 37.10.3 + Node 22.21.1 + Chrome 138 |
| 代码体量 | app.asar 共 14362 文件（main/ 主进程 181 文件、cli/ 内置 CLI（9.4MB 主包）、renderer/、node_modules/） |
| 运行实例 | 实际运行的不是 /Applications 里的包，而是 `~/Library/Caches/com.tencent.workbuddy.mac.BundleMigration/backups/WorkBuddy-5.8.2.*.backup.app`，userData=`~/.workbuddy/app` |
| 特色组件 | TuringShield.bundle（防护）、sandbox-center + betterleaks（沙箱与敏感信息检测）、editor_sdk（腾讯文档编辑内核） |

## 2. REA 使用记录（含踩坑）

1. `npx rea-agents analyze-javascript-application <asar>` 直接分析报 `artifact_integrity_mismatch`（unpacked 原生库打包后签名不符，Electron 常态）→ 加 `--integrity-policy record-and-continue`
2. macOS 上 REA 启动时无条件用 xcrun 编译 Swift 进程辅助工具，本机 CLT 的 MacOSX27.0.sdk Swift 接口有兼容 bug → **解法：`SDKROOT=.../MacOSX26.sdk`**（15.sdk 亦可）
3. 全应用分析（11360 文件）产出超 REA 返回上限（360MB）→ 缩小到 `main/` 子目录 + `--max-semantic-nodes 60000` 成功

**最终成功参数**：
```bash
SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk \
npx -y rea-agents@latest analyze-javascript-application /tmp/wb-re/app-dir/main \
  --max-semantic-nodes 60000 --json
```

## 3. REA 分析结果（主进程 174 个 JS 文件）

- 访问 388 万 AST 节点，产出 **2745 个 findings**、语义图 54993 节点 / 48880 关系
- **Electron 攻击面**：6 个 BrowserWindow、16 处自定义 webPreferences、2 个 contextBridge API（7 个暴露成员）
- **IPC 面**：92 个 IPC 操作（46 个字面量通道、84 个主进程 handler、45 处动态通道名）、24 处 sender 校验观察、**1 处未配对的 renderer 发送**（值得审计）

## 4. 凭据保护体系（人工逆向闭环，与 REA/插件侧证据互证）

```
workbuddy-desktop.info（凭据文件）
  ~/Library/Application Support/CodeBuddyExtension/Data/Public/auth/
  明文字段: uid/uin/tokenType/sessionState/scope/domain...
  敏感字段: accessToken / refreshToken / 手机号 / 昵称
    → {"$wbEncrypted":1, "envelope": base64}
      envelope 解出 = {suite, keyId(16hex), nonce(12B), authTag(16B), ciphertext}
      → AES-GCM 信封，keyId = sha256(key) hex 截 16

主密钥 blob：~/.workbuddy/keyblob（0600，336B）
  = {version:1, keyId, slots:[{type, protectorKeyId, wrapped}]}
  master key 被 protector key 包裹（wrapped slot）

密钥传递：daemon 启动时经 IPC 通道 daemon:credential-protection-bootstrap
  向主进程要 key payload {version:1, atRestSecretKey(32B b64), atRestDeveloperPublicKey}
  策略开关：env WORKBUDDY_AT_REST_ENCRYPTION（off/fields/...）
  主进程写 keyblob 于 configDir；插件只读桌面文件 + 自存刷新副本
```

结论：静态加密强度取决于 protector key 的根（存在不对称 sealed envelope 分支，开发者公钥参与），本机文件均为非明文存储；dsh-workbuddy-connect 插件选择不破解、而是启动 Electron 二进制借其自身解密——这是最现实的路子。

## 5. 产物位置

- REA 证据包：`/tmp/wb-re/rea-main-analysis.json`（107MB，含完整语义图）
- asar 完整解包：`/tmp/wb-re/app-dir/`（670MB）
- 主进程源码（可读反混淆前）：`/tmp/wb-re/main-all/`（171 文件）
- asar 工具：`/tmp/wb-re/asar-ls.mjs` / `asar-x.mjs` / `asar-extract-all.mjs`

## 6. 合规边界

仅对本机已安装、自有账号的软件做互操作性研究；全程未导出任何 token/密钥明文，敏感字段仅分析结构。
