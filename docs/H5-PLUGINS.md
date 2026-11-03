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
- 静态资源（`/plugins/<dir>/*`）免鉴权——页面本来就是公开入口，不放任何敏感信息是包作者的责任（发布校验只拦结构，内容自审）。

### 能力清单（当前全部）

| 能力 | 放行路由 |
| --- | --- |
| `state.read` | `GET /h5api/state`（任务/会话/助手可用性精简视图） |
| `tasks.read` | `GET /h5api/tasks` |
| `tasks.write` | `POST /h5api/tasks`、`POST /h5api/tasks/<id>/(confirm\|cancel)` |
| `sessions.read` | `GET /h5api/sessions` |
| `sessions.write` | `POST /h5api/sessions` |

另有 `GET /h5api/whoami` 不算能力，任何有效 token 可调（返回插件 id、能力表、过期时间）。

## 链路

- **安装**：与其它包同一条路——市场发布（`npx made-market publish`）→ App 市场页/工作台/`npx made-market install` → 桥接器解包校验（manifest 一致性 + 能力白名单 + 入口页存在，见 `plugin-install.mjs` 的 `validateH5Archive`）→ 落 `~/made/plugins/<id>/`。H5 包没有代码，不触发插件 reload。
- **打开**：App 市场页对已装的 h5 包显示「打开」→ `H5Activity`（WebView）加载 `/plugins/<dir>/`。局域网直连设备口；云端用户走云代理（WebView URL 额外带 `?cloud_token=`，云代理注入网关头）。
- **板子**：完全不感知 h5 包的存在。

## 参考实现

- `tetris-h5`（零能力纯静态）与 `task-card-h5`（全能力演示，含 `/h5api/*` 全部路由的页面端封装）在 made-market-plugins 仓库；仓库内置最小示例 `packages/h5/demo-hello`。
- 市场侧校验：made-market 仓库 `cli/lib/market.mjs` 的 h5 分支（CLI 与服务端共用一份）。

## 改能力清单的同步点（三处）

1. `made/bridge/src/h5-apps.mjs` 的 `H5_CAPABILITIES` + server.mjs 的 `/h5api/*` 路由实现；
2. `made-market/made-market/cli/lib/market.mjs` 的 `H5_CAPABILITIES`；
3. 本文档的能力表。
