# bridge

电脑侧桥接器。设备 API 在 `8788`，工作台在 `127.0.0.1:8787`。

## 启动方式

```sh
npx made-bridge        # 任何机器一条命令（npm 包，Node >= 22）
```

- 首次运行会在 `~/.made-bridge/config.local.json` 生成配置（含示例项目），编辑 `projects` 指向你自己的代码仓库后重启。
- 配置、状态、工作区都在 `~/.made-bridge/`，与代码分离——npm 包目录视为只读，缓存被清也不丢数据。
- 环境变量：`VIBE_CONFIG` / `VIBE_STATE_DIR` / `VIBE_DATA_DIR` 可把三个位置各自指到别处；`VIBE_USB_PORT`、`MADE_HOME` 等见 `npm run doctor` 输出。

```sh
npm start              # 源码仓库运行（本目录）；沿用仓库里的 config.local.json
npm run doctor         # 自检：运行环境、依赖命令、接入方式、已装插件
```

编程助手 CLI（codex / cursor / qoder 等）需要已安装在 PATH 上；刷机用仓库根的 `npm run flash`，不随 npm 包分发。

## 目录结构

内部仍是桥接器自己的运行结构：

- `src/`：服务、设备接口与内置 agent
- `public/`：工作台
- `builtin/`：随代码发布的内置插件与主题（`builtin/plugins/`、`builtin/themes/`），升级代码即升级内置包
- `examples/`：示例
- `tools/`：开发工具
- `test/`：测试

## 插件与主题的目录

用户包不在本目录，而是在固定的用户目录里：

- `~/made/plugins/` —— 管控插件与助手插件
- `~/made/themes/` —— 主题包

桥接器每次启动扫描这两个目录，市场装到这里即可生效，不需要跟配置文件放在一起。路径可用 `MADE_HOME` 环境变量或 `config.local.json` 里的 `madeHome` 改成别处。

查找顺序是「内置在前、用户目录在后」，**同名时用户目录覆盖内置**。所以 `bridge/builtin/` 里的内置包是只读的：工作台可以启用、停用，但不能移除；想覆盖就在 `~/made/` 放同名目录。

插件市场和专家市场只负责发包，不放在这里。包安装进 `~/made/` 之后，桥接器才会加载。配置文件：源码仓库运行用本目录的 `config.local.json`；`npx made-bridge` 运行用 `~/.made-bridge/config.local.json`。

在仓库根目录执行 `npm start` 或 `npm test` 会转到这里。
