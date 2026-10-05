import { DEFAULT_TRANSCRIBE_MODEL, DEFAULT_TRANSCRIBE_URL } from './voice-settings.mjs';
import { normalizeSpeechSettings, redactSpeechError, speechSelection, speechSettingsState } from './speech-settings.mjs';

const MAX_WAV_BYTES = 1_100_000;

export function validateVoiceWav(wav) {
  if (!Buffer.isBuffer(wav) || wav.length < 44 || wav.length > MAX_WAV_BYTES) {
    throw new Error('录音大小不符合要求（最多约 30 秒）');
  }
  if (wav.toString('ascii', 0, 4) !== 'RIFF' ||
      wav.readUInt32LE(4) !== wav.length - 8 ||
      wav.toString('ascii', 8, 12) !== 'WAVE' ||
      wav.toString('ascii', 12, 16) !== 'fmt ' ||
      wav.readUInt32LE(16) !== 16 ||
      wav.readUInt16LE(20) !== 1 ||
      wav.readUInt16LE(22) !== 1 ||
      wav.readUInt32LE(24) !== 16000 ||
      wav.readUInt32LE(28) !== 32000 ||
      wav.readUInt16LE(32) !== 2 ||
      wav.readUInt16LE(34) !== 16 ||
      wav.toString('ascii', 36, 40) !== 'data') {
    throw new Error('录音必须是 16 kHz、16 bit、单声道 PCM WAV');
  }
  const size = wav.readUInt32LE(40);
  if (size !== wav.length - 44 || size < 16000 || size > 16000 * 2 * 30) {
    throw new Error('录音内容长度无效（需 0.5 到 30 秒）');
  }
  return wav;
}


export function transcriptionState(config) {
  return {
    ...speechSettingsState(config),
    mode: config.speech?.pluginId || config.asrMode || 'openai',
    keySource: config.openaiKeySource || (config.openaiKey ? 'environment' : 'none'),
    url: config.transcribeUrl || DEFAULT_TRANSCRIBE_URL,
    model: config.transcribeModel || DEFAULT_TRANSCRIBE_MODEL,
  };
}

// A plugin receives an abort signal, and cancellation also stops waiting even if
// an extension accidentally ignores it. The extension must release its own I/O.
async function invoke(plugin, options) {
  options.signal.throwIfAborted();
  let aborted;
  const cancellation = new Promise((_resolve, reject) => {
    aborted = () => reject(options.signal.reason);
    options.signal.addEventListener('abort', aborted, { once: true });
  });
  try {
    return await Promise.race([Promise.resolve().then(() => {
      options.signal.throwIfAborted();
      return plugin.transcribe(options);
    }), cancellation]);
  } finally {
    options.signal.removeEventListener('abort', aborted);
  }
}

export async function transcribe(wav, config, signal) {
  signal?.throwIfAborted();
  validateVoiceWav(wav);
  const selection = speechSelection(config);
  if (!selection.plugin) throw new Error('所选语音识别插件尚未安装');
  const { plugin } = selection;
  let settings;
  let transcript;
  try {
    settings = normalizeSpeechSettings(plugin, selection.settings, { required: true });
    const boundedSignal = AbortSignal.any([AbortSignal.timeout(120000), ...(signal ? [signal] : [])]);
    transcript = await invoke(plugin, { wav, settings, signal: boundedSignal, config });
  } catch (error) {
    signal?.throwIfAborted();
    if (error?.name === 'TimeoutError') throw new Error('语音识别超时，请重试');
    throw new Error(redactSpeechError(error, plugin, settings || selection.settings));
  }
  signal?.throwIfAborted();
  if (typeof transcript !== 'string') throw new Error('语音识别插件必须返回文字');
  transcript = transcript.trim();
  if (transcript.length < 3 || transcript.length > 2000) {
    throw new Error(transcript ? '识别文字过长或过短，请重试' : '语音识别尚未配置或未听清内容');
  }
  return transcript;
}
