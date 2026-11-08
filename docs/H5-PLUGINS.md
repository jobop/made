# H5 插件（kind=h5）

装到桥接器上的自包含静态网页应用。手机 App（WebView）打开页面，数据能力走能力协商；**板子（ESP32）不参与 H5 插件**——不下发、不渲染，这是定案（板载交互组件是另一个方向，见 docs/HACHIMI-PETUI-STUDY.md）。

## 包结构

```
tetris-h5/
├── market.json     # 市场元数据（kind: "h5"）
├── plugin.json     # H5 清单：{ id, kind: "h5", www?, capabilities? }
└── www/
    └── index.html  # 入口页（必须存在）
```

`plugin.json` 字段（16KB 上限）：

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `id` | 是 | 必须与包目录名一致，`^[a-z0-9][a-z0-9_-]{0,39}$` |
| `kind` | 是 | 恒为 `"h5"` |
| `www` | 否 | 静态根目录，默认 `www/`，包内相对路径，禁 `..` |
| `capabilities` | 否 | 能力白名单子集（≤8 项），缺省 = 零能力纯静态页 |

## 能力协商（安全模型）

- **裁决点在桥接器**（`bridge/src/h5-apps.mjs` 的 `H5_CAPABILITIES`）；市场（made-market）用同名单做发布预检，两侧名单必须同步改。
- 页面本身拿不到任何桥接器凭证。打开页面时 App 用**设备令牌**调 `GET /plugins/<dir>/token` 换一张**短期 scoped token**（12 小时、进程内存态、重启失效），拼在 URL 上（`?h5token=…`）交给页面。
- 页面调数据接口 `Bearer <h5token>`（或照抄 URL 上的 `?h5token=`）。桥接器逐路由校验 token 对应包**声明过**的能力，未声明一律 403；token 失效/包被删一律 401。
- **页面调 `/h5api/*` 必须用相对路径 `../../h5api/...`，禁止根绝对路径 `/h5api/...`**：局域网页面在 `/plugins/<dir>/` 下，`../../` 正好落回根；云端页面在 `/bridge/<userId>/plugins/<dir>/` 下，`../../` 落到 `/bridge/<userId>/h5api`，由云代理转发到设备口。写死 `/h5api` 在云端会打到域名根上（云服务器无此路由），页面永远显示「未连接桥接器」（2026-11-04 task-card-h5/whack-a-mole-h5 踩坑）。`cloud_token` 查询参数云代理认，页面把 `location.search` 原样带上即可穿透鉴权。
- 静态资源（`/plugins/<dir>/*`）免鉴权——页面本来就是公开入口，不放任何敏感信息是包作者的责任（发布校验只拦结构，内容自审）。

### 能力清单（当前全部）

| 能力 | 放行路由 |
| --- | --- |
| `state.read` | `GET /h5api/state`（任务/会话/助手可用性精简视图） |
| `tasks.read` | `GET /h5api/tasks` |
| `tasks.write` | `POST /h5api/tasks`、`POST /h5api/tasks/<id>/(confirm\|cancel)` |
| `sessions.read` | `GET /h5api/sessions` |
| `sessions.write` | `POST /h5api/sessions` |
| `settings.read` | `GET /h5api/settings`（读本包配置；密钥类字段只出「有没有填」，不出值） |

另有 `GET /h5api/whoami` 不算能力，任何有效 token 可调（返回插件 id、能力表、过期时间）。

### 设置项

`plugin.json` 可声明 `settings` 数组（与助手包同一套字段规范：key/label/type=text/default/required/pattern/help，help ≤200 字节）：

```json
{ "id": "my-h5", "kind": "h5", "capabilities": ["settings.read"],
  "settings": [ { "key": "nickname", "label": "昵称", "type": "text", "default": "阿黄" } ] }
```

- 设置在 **App「桥接器设置」页**编辑（云端模式菜单「云端桥接器设置」/局域网模式菜单「插件设置」），保存落在包目录 settings.json，页面下一次拉取即生效。
- 页面声明 `settings.read` 后调 `GET /h5api/settings`：`{ plugin, settings: {...}, secretFields: [...] }`。**密钥类字段**（key 含 key/secret/token/password/passphrase）永远不出值，只出字段名列表——页面需要密钥时应走自己的服务端，不要指望从这里拿。
- App 的小程序面板（主页精灵区下拉）列出已装 H5 应用，点卡片直接打开。

## 链路

- **安装**：与其它包同一条路——市场发布（`npx made-market publish`）→ App 市场页/工作台/`npx made-market install` → 桥接器解包校验（manifest 一致性 + 能力白名单 + 入口页存在，见 `plugin-install.mjs` 的 `validateH5Archive`）→ 落 `~/made/plugins/<id>/`。H5 包没有代码，不触发插件 reload。
- **打开**：App 市场页对已装的 h5 包显示「打开」→ `H5Activity`（WebView）加载 `/plugins/<dir>/`。局域网直连设备口；云端用户走云代理（WebView URL 额外带 `?cloud_token=`，云代理注入网关头）。
- **本地缓存**（App 端，省云端额度）：`H5Activity` 用 `shouldInterceptRequest` 统一接管 `/plugins/<dir>/*` 静态请求——缓存命中直接回本地（零网络流量），未命中在线拉取并落盘。失效用 `/api/installed` 里 `h5apps[].version`（market.json 版本）：版本一致沿用缓存，不同/为空则清缓存重下；版本号在页面加载完成后才落盘（中途退出只留部分缓存，下次在线会刷新）。沙箱回收/主动离线时纯静态游戏可直接从缓存玩；`/h5api/*` 不缓存，离线时页面自显「未连接」。缓存目录 `files/h5cache/<id>/`，版本标记 `files/h5cache/<id>.ver`，单文件缓存上限 8MB。
- **板子**：完全不感知 h5 包的存在。

## 参考实现

- `tetris-h5`（零能力纯静态）与 `task-card-h5`（全能力演示，含 `/h5api/*` 全部路由的页面端封装）在 made-market-plugins 仓库；仓库内置最小示例 `packages/h5/demo-hello`。
- 市场侧校验：made-market 仓库 `cli/lib/market.mjs` 的 h5 分支（CLI 与服务端共用一份）。

## 改能力清单的同步点（三处）

1. `made/bridge/src/h5-apps.mjs` 的 `H5_CAPABILITIES` + server.mjs 的 `/h5api/*` 路由实现；
2. `made-market/made-market/cli/lib/market.mjs` 的 `H5_CAPABILITIES`；
3. 本文档的能力表。

### Android 缓存隔离

应用列表、静态资源与版本文件共用按桥接器完整 base URL（包括云端 `/bridge/<userId>` 路径）和设备配对凭据计算的 SHA-256 命名空间。不同桥接器、云账号或重新配对后的凭据不共享缓存；旧版未分区的缓存不迁移，首次在线打开后重新建立。资源拦截仅处理当前插件 URL 前缀下的 GET 请求，不能只凭路径中包含插件 ID 命中其他来源。
