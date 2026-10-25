package com.made.app;

import android.annotation.SuppressLint;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.util.Log;

import java.io.ByteArrayOutputStream;

/**
 * 真麦克风录音（16kHz 单声道 PCM16 → WAV），与固件录音参数一致
 * （桥接器 /device/voice 走同一条转写链）。
 */
public final class VoiceRecorder {
    private static final String TAG = "made.voice";
    private static final int SAMPLE_RATE = 16000;
    private static final long MAX_MS = 30000;   // 32KB/s，远低于桥接器 1.1MB 上限
    private static final long MIN_MS = 500;

    public interface LevelListener {
        /** 采集线程回调：0~1 归一化音量。 */
        void onLevel(float level);
    }

    private AudioRecord record;
    private volatile boolean recording = false;
    private Thread thread;
    private final ByteArrayOutputStream pcm = new ByteArrayOutputStream();
    private long startedAt = 0;
    private LevelListener levelListener;

    public void setLevelListener(LevelListener l) {
        levelListener = l;
    }

    public boolean isRecording() { return recording; }

    /** @return false = 麦克风不可用（权限/占用）。 */
    @SuppressLint("MissingPermission")
    public boolean start() {
        if (recording) return true;
        int minBuf = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        if (minBuf <= 0) return false;
        try {
            record = new AudioRecord(MediaRecorder.AudioSource.MIC, SAMPLE_RATE,
                    AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, minBuf * 2);
        } catch (Exception e) {
            Log.w(TAG, "AudioRecord create", e);
            return false;
        }
        if (record.getState() != AudioRecord.STATE_INITIALIZED) {
            record.release();
            record = null;
            return false;
        }
        pcm.reset();
        recording = true;
        startedAt = System.currentTimeMillis();
        record.startRecording();
        thread = new Thread(this::captureLoop, "made-mic");
        thread.start();
        return true;
    }

    private void captureLoop() {
        short[] buf = new short[1600]; // 100ms
        while (recording) {
            int n = record.read(buf, 0, buf.length);
            if (n <= 0) break;
            synchronized (pcm) {
                for (int i = 0; i < n; i++) {
                    pcm.write((byte) (buf[i] & 0xFF));
                    pcm.write((byte) ((buf[i] >> 8) & 0xFF));
                }
            }
            if (levelListener != null) {
                long sum = 0;
                for (int i = 0; i < n; i++) sum += (long) buf[i] * buf[i];
                float rms = (float) Math.sqrt(sum / (double) n) / 32768f;
                levelListener.onLevel(Math.min(1f, rms * 8f));
            }
            if (System.currentTimeMillis() - startedAt > MAX_MS) break;
        }
    }

    /** 停止并产出 WAV；时长不足返回 null。 */
    public byte[] stopAndGetWav() {
        if (!recording) return null;
        recording = false;
        try {
            thread.join(1500);
        } catch (InterruptedException ignored) { }
        if (record != null) {
            try {
                record.stop();
            } catch (Exception ignored) { }
            record.release();
            record = null;
        }
        long duration = System.currentTimeMillis() - startedAt;
        byte[] data;
        synchronized (pcm) {
            data = pcm.toByteArray();
        }
        if (duration < MIN_MS || data.length == 0) return null;
        return wavBytes(data);
    }

    public void cancel() {
        recording = false;
        try {
            if (thread != null) thread.join(1500);
        } catch (InterruptedException ignored) { }
        if (record != null) {
            try {
                record.stop();
            } catch (Exception ignored) { }
            record.release();
            record = null;
        }
    }

    private static byte[] wavBytes(byte[] pcmData) {
        int totalLen = 44 + pcmData.length;
        byte[] out = new byte[totalLen];
        // RIFF header，与固件上传的 WAV 同构（PCM16 mono 16k）。
        writeAscii(out, 0, "RIFF");
        writeInt32Le(out, 4, totalLen - 8);
        writeAscii(out, 8, "WAVE");
        writeAscii(out, 12, "fmt ");
        writeInt32Le(out, 16, 16);
        writeInt16Le(out, 20, 1);            // PCM
        writeInt16Le(out, 22, 1);            // mono
        writeInt32Le(out, 24, SAMPLE_RATE);
        writeInt32Le(out, 28, SAMPLE_RATE * 2); // byte rate
        writeInt16Le(out, 32, 2);            // block align
        writeInt16Le(out, 34, 16);           // bits
        writeAscii(out, 36, "data");
        writeInt32Le(out, 40, pcmData.length);
        System.arraycopy(pcmData, 0, out, 44, pcmData.length);
        return out;
    }

    private static void writeAscii(byte[] b, int off, String s) {
        for (int i = 0; i < s.length(); i++) b[off + i] = (byte) s.charAt(i);
    }

    private static void writeInt32Le(byte[] b, int off, int v) {
        b[off] = (byte) v;
        b[off + 1] = (byte) (v >> 8);
        b[off + 2] = (byte) (v >> 16);
        b[off + 3] = (byte) (v >> 24);
    }

    private static void writeInt16Le(byte[] b, int off, int v) {
        b[off] = (byte) v;
        b[off + 1] = (byte) (v >> 8);
    }
}
