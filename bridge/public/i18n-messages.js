// Interface messages only; user instructions and assistant replies are never translated.
export const messages = {
  "app.001": {
    "zh-CN": "等待确认",
    "en": "Awaiting confirmation"
  },
  "app.002": {
    "zh-CN": "排队中",
    "en": "Queued"
  },
  "app.003": {
    "zh-CN": "执行中",
    "en": "Running"
  },
  "app.004": {
    "zh-CN": "已完成",
    "en": "Completed"
  },
  "app.005": {
    "zh-CN": "已转交",
    "en": "Handed off"
  },
  "app.006": {
    "zh-CN": "失败",
    "en": "Failed"
  },
  "app.007": {
    "zh-CN": "已取消",
    "en": "Cancelled"
  },
  "app.008": {
    "zh-CN": "码得",
    "en": "made"
  },
  "app.009": {
    "zh-CN": "等待重试",
    "en": "Awaiting retry"
  },
  "app.010": {
    "zh-CN": "未知状态",
    "en": "Unknown status"
  },
  "app.011": {
    "zh-CN": "Codex 对话在另一窗口运行。请先停止并关闭，再次确认。",
    "en": "This Codex conversation is active in another window. Stop and close it, then confirm again."
  },
  "app.012": {
    "zh-CN": "未指定助手",
    "en": "No assistant selected"
  },
  "app.013": {
    "zh-CN": "未指定项目",
    "en": "No project selected"
  },
  "app.014": {
    "zh-CN": "此助手接口不支持指定对话；码得只归组任务，无法保证上下文隔离",
    "en": "This assistant cannot target a conversation. made groups tasks, but cannot guarantee separate context."
  },
  "app.015": {
    "zh-CN": "助手会沿用这个任务的上下文",
    "en": "The assistant continues with this task's context"
  },
  "app.016": {
    "zh-CN": "当前为独立执行；助手不会自动记住此前的指令",
    "en": "Each instruction runs independently; earlier instructions are not remembered"
  },
  "app.017": {
    "zh-CN": "请求失败（{v0}）",
    "en": "Request failed ({v0})"
  },
  "app.018": {
    "zh-CN": "桥接服务没有返回操作令牌",
    "en": "The bridge did not return an action token"
  },
  "app.019": {
    "zh-CN": "最近同步 {v0}",
    "en": "Last synced {v0}"
  },
  "app.020": {
    "zh-CN": "无法连接桥接服务",
    "en": "Cannot connect to the bridge"
  },
  "app.021": {
    "zh-CN": "已再次确认，Codex 正在重试。",
    "en": "Confirmed again. Codex is retrying."
  },
  "app.022": {
    "zh-CN": "指令已确认，正在交给编程助手。",
    "en": "Instruction confirmed and sent to the assistant."
  },
  "app.023": {
    "zh-CN": "取消请求已发送。",
    "en": "Cancellation requested."
  },
  "app.024": {
    "zh-CN": "操作失败",
    "en": "Action failed"
  },
  "app.025": {
    "zh-CN": "再次确认",
    "en": "Confirm again"
  },
  "app.026": {
    "zh-CN": "确认执行",
    "en": "Confirm and run"
  },
  "app.027": {
    "zh-CN": "取消指令",
    "en": "Cancel instruction"
  },
  "app.028": {
    "zh-CN": "确认",
    "en": "Confirm"
  },
  "app.029": {
    "zh-CN": "取消",
    "en": "Cancel"
  },
  "app.030": {
    "zh-CN": "剪贴板不可用",
    "en": "Clipboard unavailable"
  },
  "app.031": {
    "zh-CN": "{v0}已复制",
    "en": "{v0} copied"
  },
  "app.032": {
    "zh-CN": "复制失败，请选中文字手动复制。",
    "en": "Copy failed. Select the text and copy it manually."
  },
  "app.033": {
    "zh-CN": "复制{v0}",
    "en": "Copy {v0}"
  },
  "app.034": {
    "zh-CN": "在 Codex 打开",
    "en": "Open in Codex"
  },
  "app.035": {
    "zh-CN": "继续命令",
    "en": "resume command"
  },
  "app.036": {
    "zh-CN": "如果同一 Codex 对话正在桌面打开或执行，请先停止并关闭它，再由码得或桥接器继续。若状态是“等待重试”，关闭后在码得双击 BOOT，或在此页点“再次确认”。若打开链接失败，可使用继续命令。",
    "en": "If this Codex conversation is open or running on the desktop, stop and close it before continuing through made or the bridge. If it is awaiting retry, double-press BOOT or choose “Confirm again” here. If the link does not open, use the resume command."
  },
  "app.037": {
    "zh-CN": "项目路径",
    "en": "project path"
  },
  "app.038": {
    "zh-CN": "Cursor CLI 会话不在桌面聊天列表。请等当前执行结束后在终端继续；也可在 Cursor 打开同一项目查看、手动修改代码。",
    "en": "Cursor CLI conversations do not appear in the desktop chat list. Wait for the current run to finish, then resume in a terminal. You can also open the same project in Cursor to inspect and edit its code."
  },
  "app.039": {
    "zh-CN": "暂无可用助手",
    "en": "No available assistants"
  },
  "app.040": {
    "zh-CN": "等待连接",
    "en": "Waiting for connection"
  },
  "app.041": {
    "zh-CN": "暂无电脑项目",
    "en": "No computer projects"
  },
  "app.042": {
    "zh-CN": "{v0} 接口不支持指定对话；可在码得分组查看任务，但无法保证上下文隔离。",
    "en": "{v0} cannot target a conversation. made can group tasks, but cannot guarantee separate context."
  },
  "app.043": {
    "zh-CN": "助手会沿用这个任务的上下文。",
    "en": "The assistant continues with this task's context."
  },
  "app.044": {
    "zh-CN": "此助手以独立指令执行，不会自动记住此前的指令。",
    "en": "This assistant runs each instruction independently and does not remember previous instructions."
  },
  "app.045": {
    "zh-CN": "未命名任务",
    "en": "Untitled task"
  },
  "app.046": {
    "zh-CN": "待开始",
    "en": "Not started"
  },
  "app.047": {
    "zh-CN": "刚刚",
    "en": "Just now"
  },
  "app.048": {
    "zh-CN": "{v0} · 更新于 {v1}",
    "en": "{v0} · Updated {v1}"
  },
  "app.049": {
    "zh-CN": "收起",
    "en": "Collapse"
  },
  "app.050": {
    "zh-CN": "＋ 新建任务",
    "en": "＋ New task"
  },
  "app.051": {
    "zh-CN": "桥接服务暂时不可用。",
    "en": "The bridge is temporarily unavailable."
  },
  "app.052": {
    "zh-CN": "还没有新任务。旧任务可在下方“历史记录”中查看。",
    "en": "No new tasks yet. View older tasks in “History” below."
  },
  "app.053": {
    "zh-CN": "还没有任务。先选择助手和代码文件夹。",
    "en": "No tasks yet. Select an assistant and a code folder first."
  },
  "app.054": {
    "zh-CN": "正在读取任务…",
    "en": "Loading tasks…"
  },
  "app.055": {
    "zh-CN": "历史记录 · {v0} 条",
    "en": "History · {v0} items"
  },
  "app.056": {
    "zh-CN": "请选择任务",
    "en": "Select a task"
  },
  "app.057": {
    "zh-CN": "选择左侧任务，或新建一个任务。",
    "en": "Select a task on the left, or create one."
  },
  "app.058": {
    "zh-CN": "{v0} 当前接口不能指定对话。码得会将消息归在这个任务下，但无法保证助手对话的上下文隔离或续接。",
    "en": "{v0} cannot target a conversation. made groups messages under this task, but cannot guarantee separate or continued context."
  },
  "app.059": {
    "zh-CN": "请选择编程助手和代码文件夹。",
    "en": "Select an assistant and a code folder."
  },
  "app.060": {
    "zh-CN": "正在创建任务…",
    "en": "Creating task…"
  },
  "app.061": {
    "zh-CN": "任务已创建。第一句指令将成为标题。",
    "en": "Task created. Your first instruction becomes its title."
  },
  "app.062": {
    "zh-CN": "创建任务失败",
    "en": "Could not create task"
  },
  "app.063": {
    "zh-CN": "正在保存…",
    "en": "Saving…"
  },
  "app.064": {
    "zh-CN": "任务标题已更新。",
    "en": "Task title updated."
  },
  "app.065": {
    "zh-CN": "改标题失败",
    "en": "Could not rename task"
  },
  "app.066": {
    "zh-CN": "桥接服务暂时不可用",
    "en": "Bridge temporarily unavailable"
  },
  "app.067": {
    "zh-CN": "先选择或新建任务",
    "en": "Select or create a task"
  },
  "app.068": {
    "zh-CN": "每个任务都有自己的助手、代码文件夹和对话上下文。",
    "en": "Each task has its own assistant, code folder, and conversation context."
  },
  "app.069": {
    "zh-CN": "这个任务还没有指令",
    "en": "No instructions in this task yet"
  },
  "app.070": {
    "zh-CN": "在下方输入第一句话，或在码得上打开这个任务后说话。",
    "en": "Enter your first instruction below, or open this task on made and speak."
  },
  "app.071": {
    "zh-CN": "最近输入 · {v0}",
    "en": "Latest input · {v0}"
  },
  "app.072": {
    "zh-CN": "（空指令）",
    "en": "(empty instruction)"
  },
  "app.073": {
    "zh-CN": "你：{v0}",
    "en": "You: {v0}"
  },
  "app.074": {
    "zh-CN": "等待确认后开始执行。",
    "en": "Execution starts after confirmation."
  },
  "app.075": {
    "zh-CN": "已排队，等待助手启动。",
    "en": "Queued, waiting for the assistant to start."
  },
  "app.076": {
    "zh-CN": "等待助手回复。",
    "en": "Waiting for the assistant's reply."
  },
  "app.077": {
    "zh-CN": "{v0}：{v1}",
    "en": "{v0}: {v1}"
  },
  "app.078": {
    "zh-CN": "你的指令仍保留，尚未执行。请先停止并关闭桌面里打开的同一 Codex 对话，再点“再次确认”继续。",
    "en": "Your instruction is saved and has not run. Stop and close this Codex conversation on the desktop, then choose “Confirm again”."
  },
  "app.079": {
    "zh-CN": "暂无助手",
    "en": "No assistants"
  },
  "app.080": {
    "zh-CN": "请在桥接器启用助手插件",
    "en": "Enable an assistant plugin in the bridge"
  },
  "app.081": {
    "zh-CN": "连接后显示助手",
    "en": "Assistants appear after connecting"
  },
  "app.082": {
    "zh-CN": "任务 {v0}/{v1} · {v2}",
    "en": "Task {v0}/{v1} · {v2}"
  },
  "app.083": {
    "zh-CN": "暂无任务",
    "en": "No tasks"
  },
  "app.084": {
    "zh-CN": "点右上角 ＋ 新建任务",
    "en": "Tap ＋ at the top right to create a task"
  },
  "app.085": {
    "zh-CN": "待输入 · 单击 BOOT 说话",
    "en": "Ready · Press BOOT to speak"
  },
  "app.086": {
    "zh-CN": "请启动电脑桥接服务。",
    "en": "Start the bridge on your computer."
  },
  "app.087": {
    "zh-CN": "请在电脑工作台启用此助手。",
    "en": "Enable this assistant on the computer dashboard."
  },
  "app.088": {
    "zh-CN": "在电脑工作台设置语音识别。",
    "en": "Configure speech recognition on the computer dashboard."
  },
  "app.089": {
    "zh-CN": "点右上角 ＋ 新建任务，第一句话会成为标题。",
    "en": "Tap ＋ at the top right to create a task. Your first instruction becomes its title."
  },
  "app.090": {
    "zh-CN": "单击 BOOT 说话。左右滑动换助手；长按 BOOT 退出。",
    "en": "Press BOOT to speak. Swipe left or right to switch assistants; hold BOOT to exit."
  },
  "app.091": {
    "zh-CN": "{v0} · 你：{v1}",
    "en": "{v0} · You: {v1}"
  },
  "app.092": {
    "zh-CN": "\n\n更多内容请在电脑任务页查看",
    "en": "\n\nView more on the computer's Tasks page"
  },
  "app.093": {
    "zh-CN": "任务过长，请在电脑任务页核对并确认",
    "en": "This instruction is too long. Review and confirm it on the computer's Tasks page."
  },
  "app.094": {
    "zh-CN": "双击 BOOT 确认 · 三击取消",
    "en": "Double-press BOOT to confirm · Triple-press to cancel"
  },
  "app.095": {
    "zh-CN": "（无任务内容）",
    "en": "(no task content)"
  },
  "app.096": {
    "zh-CN": "已排队，等待助手启动",
    "en": "Queued, waiting for the assistant"
  },
  "app.097": {
    "zh-CN": "等待助手回复",
    "en": "Waiting for a reply"
  },
  "app.098": {
    "zh-CN": "桥接离线",
    "en": "Bridge offline"
  },
  "app.099": {
    "zh-CN": "本机桥接在线",
    "en": "Local bridge online"
  },
  "app.100": {
    "zh-CN": "正在连接",
    "en": "Connecting"
  },
  "app.101": {
    "zh-CN": "本机桥接服务",
    "en": "Local bridge service"
  },
  "app.102": {
    "zh-CN": "离线",
    "en": "Offline"
  },
  "app.103": {
    "zh-CN": "在线",
    "en": "Online"
  },
  "app.104": {
    "zh-CN": "连接中",
    "en": "Connecting"
  },
  "app.105": {
    "zh-CN": "独立语音识别",
    "en": "Speech recognition"
  },
  "app.106": {
    "zh-CN": "语音识别已就绪",
    "en": "Speech recognition ready"
  },
  "app.107": {
    "zh-CN": "请完成语音输入设置",
    "en": "Complete speech input settings"
  },
  "app.108": {
    "zh-CN": "码得通信入口",
    "en": "made connection endpoint"
  },
  "app.109": {
    "zh-CN": "已开启",
    "en": "Enabled"
  },
  "app.110": {
    "zh-CN": "未开启",
    "en": "Disabled"
  },
  "app.111": {
    "zh-CN": "码得在线",
    "en": "made devices online"
  },
  "app.112": {
    "zh-CN": "{v0} / {v1} 台",
    "en": "{v0} / {v1} devices"
  },
  "app.113": {
    "zh-CN": "码得 USB 直连",
    "en": "made direct USB"
  },
  "app.114": {
    "zh-CN": "USB 接收端",
    "en": "USB receiver"
  },
  "app.115": {
    "zh-CN": "已连接",
    "en": "Connected"
  },
  "app.116": {
    "zh-CN": "USB 连接",
    "en": "USB connection"
  },
  "app.117": {
    "zh-CN": "等待打开码得",
    "en": "Waiting for made to open"
  },
  "app.118": {
    "zh-CN": "电脑桥接器离线，暂时无法检测 USB 连接。",
    "en": "The computer bridge is offline. USB connections cannot be checked right now."
  },
  "app.119": {
    "zh-CN": "码得 USB 直连已连接{v0}。不用 Wi-Fi；首次使用请点击“添加码得”并核对六位码。",
    "en": "made is connected through USB{v0}. No Wi-Fi needed. For first-time setup, select “Add made” and compare the six-digit code."
  },
  "app.120": {
    "zh-CN": "接收端已连接{v0}。码得可连接下面的专用热点。",
    "en": "Receiver connected{v0}. made can join the dedicated hotspot below."
  },
  "app.121": {
    "zh-CN": "码得 USB 已连接，应用已退出。重新打开码得即可连接。",
    "en": "made USB is connected, but the app is closed. Open made to reconnect."
  },
  "app.122": {
    "zh-CN": "检测到 USB 串口，等待设备握手。请在码得接入设置选择“USB”并打开应用。",
    "en": "USB serial port detected; waiting for the device handshake. Select “USB” in made connection settings and open the app."
  },
  "app.123": {
    "zh-CN": "尚未连接 USB 设备：{v0}",
    "en": "No USB device connected: {v0}"
  },
  "app.124": {
    "zh-CN": "尚未连接 USB 设备。插入码得或已烧录的 ESP32-S3 / C3 接收端后会自动检测。",
    "en": "No USB device connected. Plug in made or a flashed ESP32-S3 / C3 receiver for automatic detection."
  },
  "app.125": {
    "zh-CN": "未启用 USB 连接；可使用同 Wi-Fi 或公网连接。",
    "en": "USB is disabled. You can connect through the same Wi-Fi or a public address."
  },
  "app.126": {
    "zh-CN": "当前是码得 USB 直连，不需要 SSID 和密码。使用另一块无屏接收端时，将它插入电脑，热点名称和密码才会显示在这里。配对列表的在线状态根据码得实际请求更新。",
    "en": "Direct USB needs no SSID or password. When using a separate receiver without a screen, plug it into the computer to see its hotspot details here. The pairing list shows device activity from actual requests."
  },
  "app.127": {
    "zh-CN": "独立接收端无需屏幕：用 USB 插入电脑后，这里自动显示热点名称和密码，点击“显示”查看密码。在码得内打开“齿轮 → 连接 → 接收端”，扫描并点选热点，再输入这里显示的密码并保存；无需退出到桌面设置。新固件的热点密码为 8 位数字，六位配对码随后单独核对。正常重启后无需重填。电脑上需运行本机桥接器。",
    "en": "The receiver needs no screen. Plug it into USB to see its hotspot name and password here; select “Show” to reveal the password. In made, open Settings → Connection → Receiver, scan and select the hotspot, then enter its password and save. No need to leave the app. New receiver firmware uses an eight-digit Wi-Fi password; compare the separate six-digit pairing code afterward. Details survive normal restarts. The bridge must run on the computer."
  },
  "app.128": {
    "zh-CN": "隐藏",
    "en": "Hide"
  },
  "app.129": {
    "zh-CN": "显示",
    "en": "Show"
  },
  "app.130": {
    "zh-CN": "留空使用 {v0} 默认模型",
    "en": "Leave blank to use the default {v0} model"
  },
  "app.131": {
    "zh-CN": "当前助手插件没有可在桥接器中配置的模型。",
    "en": "The installed assistant plugins have no model settings available in the bridge."
  },
  "app.132": {
    "zh-CN": "、",
    "en": ", "
  },
  "app.133": {
    "zh-CN": "{v0} 的插件不支持指定模型，请在对应助手中设置。",
    "en": "{v0} cannot select a model through their plugins; configure it in the assistant itself. "
  },
  "app.134": {
    "zh-CN": "{v0}模型名称需由对应助手账号支持。",
    "en": "{v0}Your assistant account must support the selected model."
  },
  "app.135": {
    "zh-CN": "模型选择已保存，下一条指令生效。",
    "en": "Model selection saved. It applies to the next instruction."
  },
  "app.136": {
    "zh-CN": "保存失败",
    "en": "Save failed"
  },
  "app.137": {
    "zh-CN": "桥接离线，暂时无法读取",
    "en": "Bridge offline; settings unavailable"
  },
  "app.138": {
    "zh-CN": "保存后启用兼容音频转写接口。",
    "en": "Saving enables the compatible audio transcription interface."
  },
  "app.139": {
    "zh-CN": "请填写接口 URL、API Key 和模型名。",
    "en": "Enter the endpoint URL, API Key, and model name."
  },
  "app.140": {
    "zh-CN": "已配置 · API Key 由电脑环境变量提供。",
    "en": "Configured · API Key supplied by a computer environment variable."
  },
  "app.141": {
    "zh-CN": "已配置 · 密钥保存在这台电脑。",
    "en": "Configured · Key stored on this computer."
  },
  "app.142": {
    "zh-CN": "已清除网页保存的密钥，电脑环境变量中的密钥仍在生效。",
    "en": "The key saved through this page was cleared. The computer environment variable still supplies a key."
  },
  "app.143": {
    "zh-CN": "已清除电脑保存的密钥。",
    "en": "The key stored on this computer was cleared."
  },
  "app.144": {
    "zh-CN": "语音设置已保存，下一次录音生效。",
    "en": "Speech settings saved. They apply to the next recording."
  },
  "app.145": {
    "zh-CN": "语音设置失败",
    "en": "Could not save speech settings"
  },
  "app.146": {
    "zh-CN": "{v0} 分 {v1} 秒",
    "en": "{v0} min {v1} sec"
  },
  "app.147": {
    "zh-CN": "电脑桥接暂时离线，恢复后可添加码得。",
    "en": "The bridge is offline. You can add made after it reconnects."
  },
  "app.148": {
    "zh-CN": "{v0} 台码得等待确认。请核对设备和电脑上的数字。",
    "en": "{v0} made devices await confirmation. Compare the codes on the device and computer."
  },
  "app.149": {
    "zh-CN": " · 剩余 {v0}",
    "en": " · {v0} remaining"
  },
  "app.150": {
    "zh-CN": "正在等待码得连接{v0}。USB 直连只需插线；其它方式请在码得接入设置中选好 Wi-Fi、接收端或公网地址，然后核对六位码。",
    "en": "Waiting for made to connect{v0}. Direct USB only needs a cable. For other methods, select Wi-Fi, Receiver, or a public address in made connection settings, then compare the six-digit code."
  },
  "app.151": {
    "zh-CN": "上滑解锁码得后会自动重连；锁屏和退出应用时不报告心跳，约 30 秒后显示离线。添加新设备时，点击“添加码得”。",
    "en": "Swipe up to unlock made and reconnect. Locking or exiting stops heartbeats; the device appears offline after about 30 seconds. To pair another device, choose “Add made”."
  },
  "app.152": {
    "zh-CN": "先点击“添加码得”，再在码得的接入设置中选择“USB”、同 Wi-Fi、接收端或公网，两端核对六位码。",
    "en": "Choose “Add made”, then select USB, same Wi-Fi, Receiver, or a public address in made connection settings. Compare the six-digit code on both sides."
  },
  "app.153": {
    "zh-CN": "码得 {v0}",
    "en": "made {v0}"
  },
  "app.154": {
    "zh-CN": " · 最近通信 {v0}",
    "en": " · Last seen {v0}"
  },
  "app.155": {
    "zh-CN": "移除",
    "en": "Remove"
  },
  "app.156": {
    "zh-CN": "移除 {v0} 并撤销连接",
    "en": "Remove {v0} and revoke access"
  },
  "app.157": {
    "zh-CN": "等待配对 · 码得",
    "en": "Pairing request · made"
  },
  "app.158": {
    "zh-CN": "任务 · 码得",
    "en": "Tasks · made"
  },
  "app.159": {
    "zh-CN": "助手 · 码得",
    "en": "Assistants · made"
  },
  "app.160": {
    "zh-CN": "大盘 · 码得",
    "en": "Dashboard · made"
  },
  "app.161": {
    "zh-CN": "{v0}请求连接",
    "en": "{v0} wants to connect"
  },
  "app.162": {
    "zh-CN": "接入方式：码得 USB 直连。请与码得显示的六位码核对。",
    "en": "Connection: made direct USB. Compare this six-digit code with the device."
  },
  "app.163": {
    "zh-CN": "未知",
    "en": "Unknown"
  },
  "app.164": {
    "zh-CN": "请求接入地址：{v0}。请与码得显示的接入地址一并核对。",
    "en": "Requested endpoint: {v0}. Also compare this address with the one shown on made."
  },
  "app.165": {
    "zh-CN": "本次请求有效期内",
    "en": "the request's validity period"
  },
  "app.166": {
    "zh-CN": "请在 {v0}完成确认",
    "en": "Confirm within {v0}"
  },
  "app.167": {
    "zh-CN": "已开始发现码得，请在设备上打开码得。",
    "en": "Discovery started. Open made on the device."
  },
  "app.168": {
    "zh-CN": "已结束添加设备。",
    "en": "Finished adding devices."
  },
  "app.169": {
    "zh-CN": "配对设置失败",
    "en": "Could not update pairing settings"
  },
  "app.170": {
    "zh-CN": "码得已配对，后续会自动连接。",
    "en": "made is paired and will reconnect automatically."
  },
  "app.171": {
    "zh-CN": "已拒绝这次连接。",
    "en": "Connection rejected."
  },
  "app.172": {
    "zh-CN": "配对操作失败",
    "en": "Pairing action failed"
  },
  "app.173": {
    "zh-CN": "这台码得",
    "en": "this made device"
  },
  "app.174": {
    "zh-CN": "确定移除“{v0}”吗？移除后设备需要重新配对才能控制电脑。",
    "en": "Remove “{v0}”? The device must pair again before it can control the computer."
  },
  "app.175": {
    "zh-CN": "已移除设备并撤销连接。",
    "en": "Device removed and access revoked."
  },
  "app.176": {
    "zh-CN": "移除设备失败",
    "en": "Could not remove device"
  },
  "app.177": {
    "zh-CN": "注册并安装 Tailscale，在这台电脑上登录。保存此接入方式后开启穿透，首次按提示授权 Funnel；这里无需填写 API Key，获批的 ts.net HTTPS 地址会显示在下方。",
    "en": "Sign up, install Tailscale, and sign in on this computer. Save this connection method and start the tunnel, then authorize Funnel when prompted. No API Key is needed here; the verified ts.net HTTPS address appears below."
  },
  "app.178": {
    "zh-CN": "注册并安装 ngrok Agent，从控制台复制 Authtoken 填入下方。HTTPS 地址可留空由平台分配；已有预留域名时再填写。保存后开启穿透。",
    "en": "Sign up and install ngrok Agent, then copy your Authtoken from the dashboard into the field below. Leave the HTTPS address blank for an assigned address, or enter a reserved domain. Save and start the tunnel."
  },
  "app.179": {
    "zh-CN": "注册并安装花生壳客户端，保持登录。在管理台创建指向 127.0.0.1:8788 的 HTTPS/443 映射，再将公网域名和 API Key 填入下方。桥接器只启停已有映射，不会代购套餐。",
    "en": "Sign up, install the Oray client, and keep it signed in. In its console, create an HTTPS/443 mapping to 127.0.0.1:8788, then enter the public domain and API Key below. The bridge enables or disables an existing mapping; it does not purchase a plan."
  },
  "app.180": {
    "zh-CN": "注册并将域名接入 Cloudflare，在控制台创建 Tunnel，将 HTTPS 域名转发到 http://127.0.0.1:8788；在电脑上安装 cloudflared。把域名和 Tunnel 令牌填入下方，保存后开启。",
    "en": "Sign up and add your domain to Cloudflare. Create a Tunnel in the dashboard and route the HTTPS domain to http://127.0.0.1:8788; install cloudflared on this computer. Enter the domain and Tunnel token below, save, and start it."
  },
  "app.181": {
    "zh-CN": "无需注册、域名或令牌。先安装 cloudflared，再保存设置并开启穿透；临时 HTTPS 地址会显示在下方，每次重开时可能变化。",
    "en": "No account, domain, or token is needed. Install cloudflared, save these settings, and start the tunnel. A temporary HTTPS address appears below and may change each time you restart it."
  },
  "app.182": {
    "zh-CN": "令牌已保存在这台电脑；留空可保持不变。",
    "en": "Token saved on this computer. Leave blank to keep it."
  },
  "app.183": {
    "zh-CN": "尚未保存 Tunnel 令牌。",
    "en": "No Tunnel token saved."
  },
  "app.184": {
    "zh-CN": "Authtoken 已保存在这台电脑；留空可保持不变。",
    "en": "Authtoken saved on this computer. Leave blank to keep it."
  },
  "app.185": {
    "zh-CN": "尚未保存 ngrok Authtoken。",
    "en": "No ngrok Authtoken saved."
  },
  "app.186": {
    "zh-CN": "API Key 已保存在这台电脑；留空可保持不变。",
    "en": "API Key saved on this computer. Leave blank to keep it."
  },
  "app.187": {
    "zh-CN": "尚未保存花生壳 API Key。",
    "en": "No Oray API Key saved."
  },
  "app.188": {
    "zh-CN": "关闭花生壳映射",
    "en": "Disable Oray mapping"
  },
  "app.189": {
    "zh-CN": "检查并关闭映射",
    "en": "Check and disable mapping"
  },
  "app.190": {
    "zh-CN": "清理残留路由",
    "en": "Clean up remaining route"
  },
  "app.191": {
    "zh-CN": "关闭穿透",
    "en": "Stop tunnel"
  },
  "app.192": {
    "zh-CN": "桥接服务暂时离线。",
    "en": "The bridge is temporarily offline."
  },
  "app.193": {
    "zh-CN": "接入设置有改动，请先保存，再开启穿透。",
    "en": "Connection settings changed. Save them before starting the tunnel."
  },
  "app.194": {
    "zh-CN": "Cloudflare Tunnel 已连接。请确认域名路由指向本机码得接口，再在码得上填写下方地址。",
    "en": "Cloudflare Tunnel connected. Check that the domain routes to this computer's made endpoint, then enter the address below on made."
  },
  "app.195": {
    "zh-CN": "公网接入已开启。码得打开“齿轮 → 连接 → 手机配置”，用手机扫码后粘贴下方 HTTPS 地址；也可在设备上手动填写。连接时按提示核对六位配对码。",
    "en": "Public access is enabled. On made, open Settings → Connect → Phone setup, scan with your phone, and paste the HTTPS address below. You can also enter it on the device. Verify the six-digit pairing code when prompted."
  },
  "app.196": {
    "zh-CN": "正在启动 Tailscale Funnel。首次使用请在弹出的浏览器完成授权，确认公开路由后才会显示地址…",
    "en": "Starting Tailscale Funnel. On first use, authorize it in the browser that opens. The address appears after the public route is verified…"
  },
  "app.197": {
    "zh-CN": "正在启动 ngrok，核对实际公网地址和转发目标…",
    "en": "Starting ngrok and checking the public address and forwarding target…"
  },
  "app.198": {
    "zh-CN": "正在检查花生壳映射与公网地址…",
    "en": "Checking the Oray mapping and public address…"
  },
  "app.199": {
    "zh-CN": "正在建立公网连接，地址生成后会显示在这里…",
    "en": "Opening public access. The address will appear here when ready…"
  },
  "app.200": {
    "zh-CN": "正在关闭公网连接…",
    "en": "Closing public access…"
  },
  "app.201": {
    "zh-CN": "公网连接失败，请重新开启。",
    "en": "Public connection failed. Try starting it again."
  },
  "app.202": {
    "zh-CN": "公网接入已关闭；局域网自动发现仍可使用。",
    "en": "Public access is off. Automatic discovery on the local network remains available."
  },
  "app.203": {
    "zh-CN": "公网地址已验证，可在码得上填写。",
    "en": "Public address verified. You can enter it on made."
  },
  "app.204": {
    "zh-CN": "正在建立公网连接，生成地址后可在码得上填写。",
    "en": "Opening public access. Enter the address on made once it appears."
  },
  "app.205": {
    "zh-CN": "已关闭公网接入。",
    "en": "Public access is off."
  },
  "app.206": {
    "zh-CN": "关闭结果尚未确认，请查看公网接入状态。",
    "en": "Shutdown is not yet confirmed. Check the public access status."
  },
  "app.207": {
    "zh-CN": "公网接入操作失败",
    "en": "Public access action failed"
  },
  "app.208": {
    "zh-CN": "公网接入设置已保存。",
    "en": "Public access settings saved."
  },
  "app.209": {
    "zh-CN": "公网接入设置失败",
    "en": "Could not save public access settings"
  },
  "app.210": {
    "zh-CN": "连接桥接服务后显示可用助手。",
    "en": "Connect to the bridge to see available assistants."
  },
  "app.211": {
    "zh-CN": "尚未启用助手插件。",
    "en": "No assistant plugins enabled."
  },
  "app.212": {
    "zh-CN": "正在连接桥接服务…",
    "en": "Connecting to the bridge…"
  },
  "app.213": {
    "zh-CN": "可转交任务",
    "en": "Can hand off tasks"
  },
  "app.214": {
    "zh-CN": "本机可用",
    "en": "Available locally"
  },
  "app.215": {
    "zh-CN": "暂不可用",
    "en": "Unavailable"
  },
  "app.216": {
    "zh-CN": "热点名称",
    "en": "hotspot name"
  },
  "app.217": {
    "zh-CN": "热点密码",
    "en": "hotspot password"
  },
  "app.218": {
    "zh-CN": "桥接地址",
    "en": "bridge address"
  },
  "app.219": {
    "zh-CN": "公网地址",
    "en": "public address"
  },
  "app.220": {
    "zh-CN": "已填入接口和模型；检查后点击“保存语音设置”。",
    "en": "Endpoint and model filled in. Review them, then choose “Save speech settings”."
  },
  "app.221": {
    "zh-CN": "请先选择任务，再填写下一条指令。",
    "en": "Select a task, then enter the next instruction."
  },
  "app.222": {
    "zh-CN": "正在发送指令…",
    "en": "Sending instruction…"
  },
  "app.223": {
    "zh-CN": "指令已回显，请核对并确认。",
    "en": "Instruction is ready. Review and confirm it."
  },
  "app.224": {
    "zh-CN": "指令已回显，等待你的确认。",
    "en": "Instruction received, awaiting your confirmation."
  },
  "app.225": {
    "zh-CN": "创建失败",
    "en": "Creation failed"
  },
  "tunnel.official.title": {
    "zh-CN": "官方入口",
    "en": "Official links"
  },
  "tunnel.official.signup": {
    "zh-CN": "注册账号",
    "en": "Sign up"
  },
  "tunnel.official.console": {
    "zh-CN": "管理控制台",
    "en": "Console"
  },
  "tunnel.official.download": {
    "zh-CN": "下载客户端",
    "en": "Download client"
  },
  "tunnel.official.guide": {
    "zh-CN": "配置指南",
    "en": "Setup guide"
  },
  "tunnel.official.authtoken": {
    "zh-CN": "控制台 / Authtoken",
    "en": "Console / Authtoken"
  },
  "tunnel.official.websiteSignup": {
    "zh-CN": "官网 / 注册",
    "en": "Website / Sign up"
  },
  "tunnel.official.newTab": {
    "zh-CN": "在新窗口打开官方网站",
    "en": "Open the official website in a new window"
  },
  "plug.001": { "zh-CN": "管控插件", "en": "Board plugin" },
  "plug.002": { "zh-CN": "助手插件", "en": "Assistant plugin" },
  "plug.003": { "zh-CN": "已启用", "en": "Enabled" },
  "plug.004": { "zh-CN": "已停用", "en": "Disabled" },
  "plug.005": { "zh-CN": "未就绪", "en": "Not ready" },
  "plug.006": { "zh-CN": "停用", "en": "Disable" },
  "plug.007": { "zh-CN": "启用", "en": "Enable" },
  "plug.008": { "zh-CN": "移除", "en": "Remove" },
  "plug.009": { "zh-CN": "应用到码得", "en": "Apply to made" },
  "plug.010": { "zh-CN": "删除", "en": "Delete" },
  "plug.011": { "zh-CN": "插件目录里还没有插件。把 .mjs 文件放进 bridge/plugins 后点刷新。", "en": "The plugins folder is empty. Put an .mjs file in bridge/plugins and refresh." },
  "plug.012": { "zh-CN": "主题目录里还没有主题。", "en": "The themes folder is empty." },
  "plug.013": { "zh-CN": "提示音", "en": "Chime" },
  "plug.014": { "zh-CN": "锁屏图标", "en": "Lock icon" },
  "plug.015": { "zh-CN": "锁屏背景", "en": "Lock background" },
  "plug.016": { "zh-CN": "确定移除插件「{v0}」？文件会从插件目录删除。", "en": "Remove plugin “{v0}”? Its file will be deleted from the plugins folder." },
  "plug.017": { "zh-CN": "确定删除主题「{v0}」？", "en": "Delete theme “{v0}”?" },
  "plug.018": { "zh-CN": "已下发到 {v0} 台码得", "en": "Sent to {v0} made device(s)" },
  "plug.019": { "zh-CN": "插件状态已更新", "en": "Plugin status updated" },
  "plug.020": { "zh-CN": "主题已删除", "en": "Theme deleted" },
  "plug.021": { "zh-CN": "插件已移除", "en": "Plugin removed" },
  "plug.022": { "zh-CN": "正在读取插件和主题…", "en": "Reading plugins and themes…" },
  "plug.023": { "zh-CN": "无法加载", "en": "Could not load" },
  "plug.024": { "zh-CN": "事件", "en": "Events" },
  "plug.025": { "zh-CN": "命令", "en": "Commands" }
};
