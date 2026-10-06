# 内置插件目录

跟代码一起发布的内置插件，`bridge/builtin/plugins/<名字>/` 一个目录一个包（`plugin.mjs` + `plugin.json` + `settings.json`）。

## 与用户目录的关系

桥接器按下面的顺序扫描，**同名时后者覆盖前者**：

1. `bridge/builtin/plugins/` —— 本目录，随代码升级
2. `~/made/plugins/` —— 用户目录，市场安装的插件都在这里

所以这里是只读的内置包：工作台「插件」页可以启用和停用，但**不能移除**——移除只作用于 `~/made/plugins/`。
想在本地覆盖某个内置插件，就在 `~/made/plugins/` 放一个同名目录。

用户目录可用 `MADE_HOME` 环境变量或配置里的 `madeHome` 改成别的路径。

## 加载规则

- 一个插件目录必须有 `plugin.mjs`（或 `plugin.js`）与 `plugin.json`
- 单文件 `.mjs` / `.js` 也会被加载

插件接口见 `bridge/src/plugins/contracts.d.ts`，示例在 `bridge/examples/plugins/`，配置规范见 `docs/PLUGINS.md`。
