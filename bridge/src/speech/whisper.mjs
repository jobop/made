import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';

function run(command, args, signal) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { stdio: ['ignore', 'ignore', 'pipe'], signal });
    let stderr = '';
    let killTimer;
    const killEventually = () => {
      killTimer = setTimeout(() => child.kill('SIGKILL'), 2000);
      killTimer.unref();
    };
    signal.addEventListener('abort', killEventually, { once: true });
    child.stderr.on('data', (chunk) => { stderr = `${stderr}${chunk}`.slice(-1500); });
    child.on('error', reject);
    child.on('close', (code) => {
      clearTimeout(killTimer);
      signal.removeEventListener('abort', killEventually);
      if (signal.aborted) reject(signal.reason);
      else if (code === 0) resolve();
      else reject(new Error(`本地语音识别失败（退出码 ${code}）：${stderr}`));
    });
  });
}

export default {
  apiVersion: 1,
  kind: 'speech-recognizer',
  id: 'whisper',
  label: '本机 Whisper',
  fields: [
    { key: 'command', label: 'Whisper 程序', type: 'path', required: true, default: 'whisper-cli' },
    { key: 'modelPath', label: '本机模型路径', type: 'path', required: true, placeholder: '/path/to/ggml-model.bin' },
  ],
  probe(settings) {
    const available = Boolean(settings.modelPath && fs.existsSync(settings.modelPath));
    return { available, reason: available ? '' : '请先选择已下载到电脑的 Whisper 模型' };
  },
  async transcribe({ wav, settings, signal }) {
    if (!settings.modelPath || !fs.existsSync(settings.modelPath)) throw new Error('请先选择已下载到电脑的 Whisper 模型');
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vibe-voice-'));
    const audioPath = path.join(directory, 'voice.wav');
    const outputBase = path.join(directory, 'transcript');
    try {
      fs.writeFileSync(audioPath, wav, { mode: 0o600 });
      await run(settings.command || 'whisper-cli', [
        '-m', settings.modelPath, '-f', audioPath, '-l', 'auto', '-nt', '-otxt', '-of', outputBase,
      ], signal);
      const transcriptPath = `${outputBase}.txt`;
      if (!fs.existsSync(transcriptPath)) throw new Error('本地识别未生成文字');
      if (fs.statSync(transcriptPath).size > 16000) throw new Error('本地识别文字过长，请重试');
      return fs.readFileSync(transcriptPath, 'utf8');
    } finally {
      fs.rmSync(directory, { recursive: true, force: true });
    }
  },
};
