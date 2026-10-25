# 原生 App v2 — 内网扫描 + 交互重做

## 完成了什么
1. **内网自动扫描连接**：复用桥接器既有 UDP 发现协议（8789/udp `VIBE_DISCOVER_V3`），手机进入连接页自动广播扫描，发现的桥接器显示为「MacBookAir · 192.168.50.75 · 8788」卡片，点一下即开始配对。桥接器端零改动；本机已验证 UDP 应答正常。AP 隔离网络收不到广播时，保留手动输入兜底。
2. **交互整套重做**（协议层 Bridge/Pairing/Poller/SpriteView/VoiceRecorder 未动）：
   - 连接页：品牌区 → 雷达动效扫描卡（重新扫描按钮、结果列表）→「找不到？手动添加」软卡（IP/端口/HTTPS）。
   - 配对卡：连接中的电脑名 + 52sp 大字配对码 + 状态反馈 + 取消/重试。
   - 主页：顶栏双行（助手名 / 当前会话，点按弹选择框，右侧 ⋯ 菜单含切换会话/新建/断开）→ 精灵区 + 底部状态胶囊（待机/工作中/等你确认/完成/出错，色彩语义）→ 最新任务卡（状态色点 + 行内「确认执行/取消」）→ 底部输入栏（圆角输入 + 圆形麦克风按住说话 + 录音时整条切换「● 00:03 松开发送，上滑取消」）。
   - 设计 token：暖灰底 #F7F5F2 + 品牌橙 #FF6C3E，8px 基线，按压 scale 0.96 微交互（仅 transform）。

## 关键文件
- 新增：`android-native/app/src/main/java/com/made/app/Ui.java`、`LanScanner.java`
- 重写：`MainActivity.java`（~740 行）；`Pairing.java` 加 `cancelPending()`
- 产物：`dist/made-android-native.apk`（86KB，versionCode 1 覆盖安装）

## 备注 / 后续
- UDP 广播在部分路由器 AP 隔离下不可用 → 手动输入路径保留。
- 语音识别结果仍以 Toast 呈现，后续可升级为把转写文本插入会话气泡。
- 真机验证项：扫描列表出现 MacBookAir、点按配对、大字配对码、主页各状态胶囊切换。
