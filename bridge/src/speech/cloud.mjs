// 云端语音识别：云端码得沙箱内的默认识别通道。
// 音频直接交给云侧 LLM 网关（/audio/transcriptions），真实供应商密钥不出云。
import { cloudLlmEnv } from '../cloud-llm.mjs';

export default {
  apiVersion: 1,
  kind: 'speech-recognizer',
  id: 'cloud',
  label: '云端语音识别',
  fields: [],
  probe() {
    return cloudLlmEnv()
      ? { available: true }
      : { available: false, reason: '仅云端码得的沙箱内可用' };
  },
  async transcribe({ wav, signal }) {
    const cloud = cloudLlmEnv();
    if (!cloud) throw new Error('云端语音识别不可用：当前不在云端码得沙箱内');
    const data = new FormData();
    data.set('model', 'cloud-asr');
    data.set('file', new Blob([wav], { type: 'audio/wav' }), 'voice.wav');
    let response;
    try {
      response = await fetch(`${cloud.baseUrl}/audio/transcriptions`, {
        method: 'POST', signal: AbortSignal.any([AbortSignal.timeout(90000), signal]),
        headers: { Authorization: `Bearer ${cloud.token}` }, body: data, redirect: 'error',
      });
    } catch {
      signal.throwIfAborted();
      throw new Error('无法连接云端语音识别服务，请重试');
    }
    if (!response.ok) {
      const detail = await response.json().then((obj) => obj?.error).catch(() => '');
      throw new Error(`云端语音识别失败（HTTP ${response.status}）${detail ? `：${detail}` : ''}`);
    }
    let result;
    try { result = await response.json(); }
    catch { throw new Error('云端语音识别未返回 JSON 结果'); }
    if (typeof result.text !== 'string') throw new Error('云端语音识别未返回文字');
    return result.text.trim();
  },
};
