import { DEFAULT_TRANSCRIBE_MODEL, DEFAULT_TRANSCRIBE_URL, validateTranscribeModel, validateTranscribeUrl } from '../voice-settings.mjs';
import { validateOpenAIKey } from '../openai-key.mjs';

export default {
  apiVersion: 1,
  kind: 'speech-recognizer',
  id: 'openai',
  label: 'OpenAI 兼容语音接口',
  fields: [
    { key: 'url', label: '转写接口 URL', type: 'url', required: true, default: DEFAULT_TRANSCRIBE_URL },
    { key: 'apiKey', label: 'API Key', type: 'secret', required: true },
    { key: 'model', label: '模型名', type: 'model', required: true, default: DEFAULT_TRANSCRIBE_MODEL },
  ],
  presets: [
    { id: 'openai', label: 'OpenAI', values: { url: DEFAULT_TRANSCRIBE_URL, model: DEFAULT_TRANSCRIBE_MODEL } },
    { id: 'siliconflow', label: '硅基流动', values: { url: 'https://api.siliconflow.cn/v1/audio/transcriptions', model: 'FunAudioLLM/SenseVoiceSmall' } },
  ],
  validateSettings(settings) {
    return { ...settings, url: validateTranscribeUrl(settings.url),
      model: validateTranscribeModel(settings.model), apiKey: validateOpenAIKey(settings.apiKey || '') };
  },
  probe(settings) {
    return { available: Boolean(settings.apiKey), reason: settings.apiKey ? '' : '请在电脑页面配置语音 API Key' };
  },
  async transcribe({ wav, settings, signal }) {
    if (!settings.apiKey) throw new Error('请在电脑页面配置语音 API Key');
    const data = new FormData();
    data.set('model', settings.model);
    // Let the service detect speech language; interface locale is independent.
    data.set('file', new Blob([wav], { type: 'audio/wav' }), 'voice.wav');
    let response;
    try {
      response = await fetch(settings.url, {
        method: 'POST', signal: AbortSignal.any([AbortSignal.timeout(90000), signal]),
        headers: { Authorization: `Bearer ${settings.apiKey}` }, body: data, redirect: 'error',
      });
    } catch {
      signal.throwIfAborted();
      throw new Error('无法连接语音识别服务，请检查电脑侧接口 URL 和网络');
    }
    if (!response.ok) throw new Error(`语音识别服务返回 ${response.status}；请检查电脑侧 URL、API Key 和模型`);
    let result;
    try { result = await response.json(); }
    catch { throw new Error('语音识别服务未返回 JSON 结果'); }
    // The host checks type and transcript bounds for every recognizer.
    return result?.text;
  },
};
