# 开发桥接器插件

桥接器管理助手和语音识别；码得是通用交互终端。新增遵守 v1 协议的插件，只安装电脑侧插件并重启桥接器，码得在重新连接或下一次目录刷新（约 31 秒）后显示。任务仍然是一个持续对话的 session，标题取第一句；后续输入继续相同上下文。

## 安装本地插件

在 `bridge/config.local.json` 中加入这些字段，保留已有 projects 等设置。下面的路径相对该配置文件，仍指向桥接器里的示例：

```json
{
  "plugins": ["./examples/plugins/echo-agent.mjs"],
  "disabledPlugins": [],
  "pluginSettings": {}
}
```

路径相对配置文件解析。支持 `.mjs`、`.js` 的 default export：单个插件对象或插件数组。`bridge/plugins/` 里的插件目录和单文件也会自动加载。不自动下载或安装依赖。插件是本机 JavaScript，拥有桥接器进程的权限，因此只加载你信任的代码。安装变更需重启电脑桥接器，不需再次配对。

示例 `echo-example` 仅回显文字和上一句，用于验证新助手、持久上下文、进度和取消，不执行编程任务。

`disabledPlugins` 可填写内置或外部助手插件 ID。禁用后历史任务与配置保留，重新安装同 ID 插件后可继续。不要复用他人 ID 来表示不同助手，否则旧上下文会被错误解释。默认助手必须安装且启用。

## 编程助手

完整接口见 `bridge/src/plugins/contracts.d.ts`，内置实现在 `bridge/src/agents/`。已安装的外部插件留在 `bridge/plugins/`，由桥接器自动加载，并在工作台「插件」页启用、停用或移除。这个目录里的插件文件不进入 git。主题包在 `bridge/themes/`，同一页管理，波波主题也在其中。插件市场只负责发包，不代替这个运行目录。

必填元数据：`apiVersion: 1`、`kind: 'coding-agent'`、唯一 `id`、`label`、`capabilities`。ID 为 1–40 个小写字母、数字、下划线、连字符（首字符字母或数字）。名称至多 64 UTF-8 字节。所有插件共享 ID 命名空间。

- `probe(config)`：同步返回 `{available, reason?, mode?}`。短时间完成，不在轮询中进行长连接或下载。
- `run({job, project, signal, config, model, onProgress, onSession})`：异步完成一轮，返回 `{status:'completed', result, gitSnapshot?}`。`job.instruction` 为这一轮文字，`project.path` 是用户选择的项目。
- `onSession(id)`：创建底层会话后立即调用，桥接器持久保存。后续同任务将通过 `job.sessionId` 传回来，插件必须恢复此会话；不得每轮创建新会话。
- `onProgress(text)`：简短用户可见的进度，例如“正在修改文件”。不得上报密钥、内部思考或整段工具输出。
- `signal`：取消时立即停止网络、子进程和后续写入；取消不得仅隐藏界面。桥接器不会自动赋予插件任何执行审批权限。
- `model`：本轮启动时固定的模型值，来自助手页面；配置改变只影响之后启动的轮次。

能力字段：

- `session: 'native'` 表示可以恢复单独任务上下文；`external_unscoped` 表示外部应用接收任务但无法隔离项目/会话，界面会明确提示这一限制。
- `model` 表示可指定模型；设为 true 后可提供 `model:{default:'',required:false}`。助手页面自动生成配置输入。
- `cancel` 表示执行中可以真正终止；不支持时界面不会承诺已取消执行。
- `progress` 表示可推送中间状态；不支持时仍可返回最终结果。

原生会话 ID 默认接受安全的有限长度不透明字符串，也可用 `sessionIdPattern` 声明自己的格式（不能使用 g/y 标志）。会话 ID 必须与插件一一对应，不能是密钥。

转交型插件可返回 `status:'handed_off'` 与 `handoff:{cursor,readState,replyCount}`，并实现 `readUpdates({job,config})`，返回新的 `handoff` 和 `appendResult`。桥接器将定期获取后续回复。共享外部回复流的插件必须自行判断消息归属；无法确定时停止回读并标为 ambiguous，不得把其他对话当作本任务结果。

需要让别人配置的插件，把配置规范放在插件自己的目录里，不要写进桥接器的 `config.local.json`。密钥仍用环境变量或系统密钥存储，不要放进可分发的配置。

## 插件配置规范

一个可发行的插件是一个目录，桥接器从 `bridge/plugins/<名字>/` 加载：

```
alarm-clock/
  plugin.mjs      插件程序
  plugin.json     配置规范：有哪些项、名称、是否必填、默认值和格式
  settings.json   当前配置值，由工作台按规范写入
```

`plugin.json` 只描述配置，不代替插件程序里的 `id`。若写了 `id`，必须和程序里的 `id` 一致。

```json
{
  "id": "alarm-clock",
  "settings": [
    {
      "key": "demoSeconds",
      "label": "演示延迟（秒）",
      "type": "text",
      "help": "每隔这么多秒响一次。留空则改用每天时间。保存后，已连上的码得会重新计时。",
      "pattern": "^(?:|[1-9]\\d{0,3})$"
    },
    {
      "key": "label",
      "label": "响铃文字",
      "type": "text",
      "default": "闹钟"
    }
  ]
}
```

规范里每一项必须有 `key`、`label` 和 `"type": "text"`。`key` 是字母开头的标识，可以含数字和下划线。`required` 为 true 时不能留空。`default` 是缺省字符串。`help` 是给安装者看的说明。`pattern` 是不带 g/y 标志的正则，桥接器用它检查 `settings.json` 里的值。

桥接器读取 `settings.json`，缺项补上 `default`，检查通过后放进 `config.pluginSettings[插件ID]`。插件只读这个对象，不自己打开配置文件。在工作台「插件」页点进该插件后，按 `plugin.json` 画出输入框，保存时写回该插件目录的 `settings.json`。已经连上的码得会立刻按新值重新安排；还没连上的，等下次连上再用。

没有 `plugin.json` 的单个 `.mjs` 文件仍可放进 `plugins/` 运行，但没有可发行的配置说明。`config.pluginSettings` 只作为这种旧文件的补充；目录里已有规范时，以插件目录为准。

插件可读取 `config.locale`（`zh-CN` 或 `en`）为自己的可用性说明和进度文案选择语言；桥接器不会对第三方插件的名称、提示或原始答复做全文替换。稳定的 `id`、会话标识与图标协议均不受语言影响。界面本地化约定见 [国际化开发说明](I18N.md)。

## 管控插件

事件和命令分开走。码得 `POST /device/events` 只上报已登记事件，响应是 `{accepted:true}`，不携带命令。插件在 `onEvent` 里返回 `{commands}`，或调用上下文里的 `send(command)`，两条路都把命令放进该设备的队列。`send` 在 `onEvent` 返回后仍然有效，可以稍后调用。命令名必须同时出现在板侧清单和插件自己的 `commands` 里；字段沿用事件字段的限制。

桥接器也可以不经过事件直接下发。本机工作台 `POST /api/board/commands`，请求体为 `{deviceId,name,fields}`，并带本机操作令牌。这条路径只要求命令在板侧清单里。

播放语音时，语音数据和命令一起入队。插件调用 `send({name:'audio.play'}, wav)`。工作台则 `POST /api/board/commands?deviceId=...`，`Content-Type` 为 `audio/wav`，正文就是 PCM WAV（16-bit，8k–48k，单声道或双声道，不超过 256KB）。码得领取命令时若看到 `audio:true`，再 `GET /device/commands/<id>/audio` 取走同一段语音并播放。

当前板侧会执行的命令：

| 命令 | 字段 | 作用 |
| --- | --- | --- |
| `caption.show` | `text` | 显示一行提示 |
| `audio.play` | 无；附带 WAV | 播放下发的语音 |
| `theme.apply` | `name` | 切换主题；`default` 恢复内置主题并重启 |
| `volume.set` | `level` | 音量 0–100 |
| `agent.select` | `id`，或 `direction`=`next`/`prev` | 切换助手 |
| `session.create` | 无 | 为当前助手新建任务 |
| `session.select` | `id`，或 `direction`=`next`/`prev` | 切换任务 |
| `session.delete` | `id` | 删除当前助手下的任务 |
| `voice.start` | 无 | 开始录音并上传 |
| `task.confirm` | 可选 `id` | 确认当前可见任务 |
| `task.cancel` | 可选 `id` | 取消当前可见任务 |

码得用 `GET /device/commands` 领取，执行后 `POST /device/commands/ack`，请求体为 `{ids}`。未确认的命令留在队列里，下次领取会再次带上。队列最多 8 条，其中待播放语音同时只保留一条。USB、局域网、接收器和公网都走这同一套设备接口。配网、配对和语言不在命令里。

## 语音识别采用固定兼容接口

语音识别不开放插件安装，只需填写 **完整转写 URL、API Key、model**。兼容 OpenAI Audio Transcriptions 的服务共用一个内置客户端，不按厂商或模型开发插件。保存后下一次录音生效，不需重启桥接器或更新固件。

请求为 POST 音频转写 URL、Bearer API Key、multipart/form-data 的 file 与 model，响应为带 text 字段的 JSON。[OpenAI 官方说明](https://developers.openai.com/api/docs/guides/speech-to-text)和[硅基流动官方接口](https://docs.siliconflow.cn/docs/api/audio-transcriptions-post)提供这一格式。兼容性取决于服务接口，不能把“模型可用于语音识别”或“兼容聊天接口”等同于“兼容音频转写接口”。当前范围是录完上传的文件转写，不是实时音频流。

密钥只保存在电脑端（0600），状态接口不返回密钥；输入留空保留原值，清除须显式操作。保留原设置迁移以及内部 WAV 校验、取消、超时处理。旧版本本机 Whisper/关闭状态仅保留内部兼容；当前网页统一配置兼容接口，不再提供语音插件选择或安装。新增非兼容服务不在当前范围内。

## 码得协议与兼容

已配对码得从 `/device/config` 获取 `providers` 数组：`id/label/icon/available/reason/capabilities`。电脑与码得都按稳定 ID 关联任务，列表顺序不决定任务归属。最多启用 12 个助手，超出时桥接器给出明确配置错误。

助手名单、名称、能力以及图标像素全部由当前已验证身份的桥接器下发。固件没有默认助手名单或动物绘图模板；连接前显示连接/配对状态，桥接器返回空目录时显示“暂无助手”。换电脑、重新进入或重新连接时先隐藏旧目录，拿到新的有效配置后再显示。目录顺序变化不影响任务归属。码得配置只携带默认项目的简短信息，并限制整个响应不超过 32 KB；电脑端项目列表仍然完整。

### 下发自定义图片

插件的 `icon` 可以直接提供如下图像对象。桥接器、网页和码得使用同一份像素数据，新增图案不需要修改固件：

```js
import icon from './my-icon.json' with { type: 'json' };
export default { /* 其他插件字段 */ icon };
```

仓库提供图片转换工具（仅开发时依赖 Pillow，桥接器运行时无需安装）：

```sh
python3 -m pip install Pillow
python3 bridge/tools/convert-agent-icon.py avatar.png my-icon.json
```

工具保留宽高比例和透明背景，缩放为 48×48 并量化到最多 16 色。`bridge/examples/plugins/echo-agent.mjs` 与 `echo-icon.json` 是使用独立图案的完整示例。

图像协议 `indexed4`：`width`、`height` 各为 1–48；`palette` 是 1–16 个 `RRGGBBAA` 八位十六进制字符串（无 `#`，含透明度）；`data` 是标准 Base64。像素按从左到右、从上到下线性排列，一字节存两个调色板索引，高四位在前，行间没有补齐；总像素数为奇数时最后低四位必须为零。`background`、`accent` 为 `#RRGGBB`，用于头像底色和强调色。最大图像数据为 1152 字节，Base64 后为 1536 字符。

桥接器加载插件时严格校验尺寸、调色板和数据长度。旧插件的 `fox/rabbit/owl/panda` 字符串仅在电脑端转换成实际像素；缺少图标时电脑端生成通用对话气泡。固件收到损坏图标时显示中性占位，其他助手与任务仍可使用。

固件联网线程只更新目录数据，界面线程统一绘制；只解码当前头像，显示缓存固定有界。原 Wi-Fi / 公网 / USB / 可选接收端传输、配对、前台心跳与 BOOT 操作保持相同协议。此次迁移到完整动态目录和图像协议需要更新一次固件，后续新增助手或更换图片无需再刷。新的硬件能力、页面交互或不兼容的协议扩展仍需更新固件。

## 验证

运行 `npm test`。测试使用临时目录和模拟执行器，不提交真实编程任务、不调用收费语音服务。新增插件至少验证：可用性、同任务恢复上下文、模型选择、进度、真实取消、重启恢复。语音客户端验证三项配置保存、密钥脱敏、取消、错误和 WAV 上传。

开发接口为 v1；不兼容扩展应提升 apiVersion，避免旧固件/桥接器误解能力。当前加载器遇到版本、重复 ID 或接口错误会明确拒绝启动。
