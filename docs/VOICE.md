# 语音识别配置

码得的麦克风录音以 16 kHz 单声道 WAV 发给电脑桥接器，由桥接器调用**兼容 OpenAI Audio Transcriptions 协议**的转写服务。语音识别不做插件化：只需填写**完整转写 URL、API Key、模型名**，兼容服务和模型共用内置转写客户端。设置和密钥只保存在电脑端，不写入码得。

## 接口要求

转写服务必须支持：

- HTTP POST + Bearer API Key 鉴权；
- `multipart/form-data`，含 `file`（WAV 音频）与 `model` 字段；
- 返回 JSON 含 `text` 字段。

「模型可用于语音识别」或「兼容聊天接口」不等于「兼容音频转写接口」，以服务实际文档为准。当前范围是**录完上传的文件转写**，不是实时音频流。

## 配置步骤

1. 启动桥接器（`npm start`），打开工作台 `http://127.0.0.1:8787`。
2. 在「语音输入设置」填写转写 URL、API Key、模型名，点「保存语音设置」。「OpenAI」「硅基流动」按钮可快速填入 URL 与模型（API Key 仍需自己的）；默认 OpenAI 端点 `https://api.openai.com/v1/audio/transcriptions`。
3. 保存后下一次录音即生效，不需重启桥接器或刷固件；板端会自动刷新语音可用状态。

也可以在启动前用环境变量：

```sh
export OPENAI_API_KEY='你的 API 密钥'
npm start
```

参考实现：[OpenAI 官方文档](https://developers.openai.com/api/docs/guides/speech-to-text)、[硅基流动音频转写](https://docs.siliconflow.cn/docs/api/audio-transcriptions-post)。已用硅基流动 `FunAudioLLM/SenseVoiceSmall` 在实板验证录音、上传、确认与取消全链路；OpenAI 真实转写尚未测试。

## 本机离线识别（Whisper）

不依赖云服务时，可安装 [whisper.cpp](https://github.com/ggml-org/whisper.cpp) 的 `whisper-cli` 与中文模型（如 `ggml-base.bin`，**不要用 `.en` 模型**），再启动：

```sh
export VIBE_ASR_MODE=whisper
export VIBE_WHISPER_MODEL='/你的模型绝对路径/ggml-base.bin'
npm start
```

## 安全与行为边界

- 密钥只存电脑端（权限 0600），状态接口不返回密钥；输入留空保留原值，清除须显式操作。
- 云服务可能产生费用，以所选服务商方案为准。
- 未配置语音服务时，电脑工作台的文字任务仍可用；码得状态文字会提示到工作台配置。
- 识别结果先在任务内回显为待确认的话，**不会自动执行**；双击 BOOT 确认、三击取消。
- 界面语言不限定录音语言，也不对识别强加中英文；识别能力取决于所选服务与模型。

## 板端录音参数

- 单击 BOOT 开始录音，结束约 1.2 秒静音自动上传；开始后 8 秒内无声报错不上传，最长 30 秒。
- 录音必须归入当前选中任务；未选任务不提交语音。
- 指令超板端接收长度时，码得阻止确认并提示到电脑任务页核对全文；答复在板端最多 1200 UTF-8 字节，超长保留首尾、中间省略。
