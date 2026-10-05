import openai from './openai.mjs';
import whisper from './whisper.mjs';

const off = {
  apiVersion: 1, kind: 'speech-recognizer', id: 'off', label: '关闭语音识别', fields: [],
  probe: () => ({ available: false, reason: '语音识别已关闭' }),
  transcribe: async () => '',
};

export const builtinSpeechRecognizers = [openai, whisper, off];
