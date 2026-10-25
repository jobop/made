package com.made.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Movie;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;

/**
 * 精灵显示，两种 manifest 形态都支持（docs/SPRITES.md，语义对齐固件
 * made_sprite.cpp）：
 * 1. frames 整帧状态动画表：{frames:{idle:[...],blink:{frames,period_ms,hold_ms}}, frame_ms}
 * 2. layers 分层叠加：layers[{id,frames[],x,y,z,blink{period_ms,closed_ms},talk}]
 *    + emotions{状态:{层:帧序号|"cycle"}} + canvas{w,h}
 * 动效对齐 artUpdate：呼吸缩放（motion.breath，默认 3s ±4%）+ 轻摆/弹跳
 * （随情绪调节奏）+ waiting 身后光圈脉冲。
 * 行动系统：actions 表（帧轨 / GIF 整帧）+ commands 命令映射，playAction
 * 播放，播完回到状态驱动；不可用动作静默忽略（降级语义）。
 * 状态名：idle/talking/running/waiting/success/sad/offline。
 */
public final class SpriteView extends View {
    private static final String TAG = "made.sprite";
    private static final long CYCLE_MS = 180; // 固件 talk/cycle 节奏

    /** 动作/命令名单变化（清单上报用），UI 线程回调。 */
    public interface ActionsListener {
        void onActionsChanged(List<String> commandNames);
    }

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Paint haloPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final Bridge bridge;
    private final Map<String, Bitmap> bitmaps = new HashMap<>();
    private final Map<String, Movie> movies = new HashMap<>();
    private ActionsListener actionsListener;
    private List<String> lastAnnounced = new ArrayList<>();

    // ---- frames 表形态 ----
    private final Map<String, List<String>> statusFrames = new HashMap<>();
    private final Map<String, Long> frameInterval = new HashMap<>(); // 每帧毫秒
    private long defaultFrameMs = 160;

    // ---- layers 分层形态 ----
    private static final class Layer {
        String id = "";
        List<String> frames = new ArrayList<>();
        int x, y, z;
        boolean blink, talk, alert;
        long blinkPeriod = 4000, blinkClosed = 120;
    }

    /** 情绪选择：帧序号（>=0）或整层循环（frame=-1 且 cycle）。 */
    private static final class Sel {
        int frame = -1;
        boolean cycle;
    }

    private final List<Layer> layers = new ArrayList<>();
    private final Map<String, Map<String, Sel>> emotions = new HashMap<>();
    private boolean layeredMode = false;
    private int canvasW = 240, canvasH = 240;

    // ---- 动效（对齐固件 MotionSpec 默认值） ----
    private long breathPeriod = 3000;
    private float breathAmp = 0.04f;   // (pct-100)/200
    private long swayPeriod = 1800;
    private float swayAmp = 7;         // 设计 px
    private float bounceAmp = 8;       // 设计 px

    // ---- 行动系统 ----
    private static final class Action {
        String gif = "";
        long gifMs = 3000;
        long frameMs = 120;
        int loops = 1;
        Map<String, List<Integer>> tracks = new HashMap<>(); // 层 id → 帧序
    }

    private final Map<String, Action> actions = new HashMap<>();
    private final Map<String, String> commandMap = new HashMap<>(); // A1..A9 → 动作名
    private String activeAction = "";
    private long actionStartMs = 0;

    private String provider = "";
    private long blinkPeriodMs = 0, blinkHoldMs = 120;

    private String status = "idle";
    private int frameIndex = 0;
    private long frameStart = 0;
    private long motionStart = 0; // 呼吸/摆动时钟（不随状态重置）
    private boolean loading = false;
    private String loadedKey = "";
    private String label = "码得";

    private final Runnable ticker = new Runnable() {
        @Override public void run() {
            if (!layeredMode && !currentFrames().isEmpty()) {
                long dur = frameInterval.containsKey(status) && frameInterval.get(status) > 0
                        ? frameInterval.get(status) : defaultFrameMs;
                if (System.currentTimeMillis() - frameStart >= dur) {
                    frameStart = System.currentTimeMillis();
                    frameIndex = (frameIndex + 1) % currentFrames().size();
                }
            }
            invalidate();
            ui.postDelayed(this, 33);
        }
    };

    public SpriteView(Context context, Bridge bridge) {
        super(context);
        this.bridge = bridge;
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setColor(0xFF888888);
        motionStart = System.currentTimeMillis();
        ui.postDelayed(ticker, 33);
    }

    @Override protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
        ui.removeCallbacks(ticker);
    }

    public void setActionsListener(ActionsListener l) { actionsListener = l; }

    public void setLabel(String text) {
        label = text;
        invalidate();
    }

    /** 清单上报名单：声明了 commands 表用命令号，否则动作语义名（老素材兼容）。 */
    public List<String> availableCommandNames() {
        List<String> out = new ArrayList<>();
        if (!commandMap.isEmpty()) {
            List<String> keys = new ArrayList<>(commandMap.keySet());
            java.util.Collections.sort(keys);
            out.addAll(keys);
        } else {
            out.addAll(actions.keySet());
        }
        return out;
    }

    private void announceActions() {
        if (actionsListener == null) return;
        List<String> names = availableCommandNames();
        if (names.equals(lastAnnounced)) return;
        lastAnnounced = new ArrayList<>(names);
        actionsListener.onActionsChanged(names);
    }

    /** 切换到某 provider 的素材（version 变化触发重下）。 */
    public void setProvider(String providerId, String spriteVersion) {
        String key = providerId + "@" + spriteVersion;
        if (key.equals(loadedKey)) return;
        loadedKey = key;
        provider = providerId;
        bitmaps.clear();
        movies.clear();
        statusFrames.clear();
        frameInterval.clear();
        layers.clear();
        emotions.clear();
        actions.clear();
        commandMap.clear();
        activeAction = "";
        layeredMode = false;
        blinkPeriodMs = 0;
        setStatus("idle");
        if (bridge != null && !providerId.isEmpty() && !spriteVersion.isEmpty()) {
            loadManifest();
        } else {
            invalidate();
        }
    }

    public void setStatus(String s) {
        if (s.equals(status)) return;
        status = s;
        frameIndex = 0;
        frameStart = System.currentTimeMillis();
        invalidate();
    }

    /** 播动作（参数为命令号或语义名），不可用静默忽略。 */
    public boolean playAction(String cmdOrName) {
        String name = commandMap.containsKey(cmdOrName) ? commandMap.get(cmdOrName) : cmdOrName;
        Action a = actions.get(name);
        if (a == null) return false; // 降级语义：不报错不动
        activeAction = name;
        actionStartMs = System.currentTimeMillis();
        invalidate();
        return true;
    }

    private List<String> currentFrames() {
        List<String> direct = statusFrames.get(status);
        if (direct != null && !direct.isEmpty()) return direct;
        List<String> idle = statusFrames.get("idle");
        return idle == null ? new ArrayList<>() : idle;
    }

    // ---------- 下载 ----------

    private void loadManifest() {
        if (loading) return;
        loading = true;
        new Thread(() -> {
            try {
                Bridge.Resp r = bridge.get("/device/sprite/" + provider + "/manifest.json");
                if (!r.ok()) {
                    Log.w(TAG, "manifest http=" + r.code);
                    loading = false;
                    return;
                }
                JSONObject m = new JSONObject(r.body);
                Map<String, List<String>> frames = new HashMap<>();
                Map<String, Long> intervals = new HashMap<>();
                long defMs = m.optLong("frame_ms", 160);
                long blinkPeriod = 0, blinkHold = 0;
                JSONObject framesObj = m.optJSONObject("frames");
                if (framesObj != null) {
                    Iterator<String> keys = framesObj.keys();
                    while (keys.hasNext()) {
                        String state = keys.next();
                        Object v = framesObj.get(state);
                        List<String> files = new ArrayList<>();
                        long perFrame = 0;
                        if (v instanceof JSONArray) {
                            JSONArray arr = (JSONArray) v;
                            for (int i = 0; i < arr.length(); i++) {
                                String f = arr.optString(i);
                                if (!f.isEmpty()) files.add(f);
                            }
                        } else if (v instanceof JSONObject) {
                            JSONObject o = (JSONObject) v;
                            JSONArray arr = o.optJSONArray("frames");
                            if (arr != null) {
                                for (int i = 0; i < arr.length(); i++) {
                                    String f = arr.optString(i);
                                    if (!f.isEmpty()) files.add(f);
                                }
                            }
                            perFrame = o.optLong("ms", 0);
                            long period = o.optLong("period_ms", 0);
                            if ("blink".equals(state)) {
                                blinkPeriod = period;
                                blinkHold = o.optLong("hold_ms", 120);
                            }
                        }
                        if (!files.isEmpty()) {
                            frames.put(state, files);
                            if (perFrame > 0) intervals.put(state, perFrame);
                        }
                    }
                }
                // 分层形态。
                List<Layer> ly = new ArrayList<>();
                Map<String, Map<String, Sel>> emo = new HashMap<>();
                int cw = 240, ch = 240;
                JSONObject canvas = m.optJSONObject("canvas");
                if (canvas != null) {
                    cw = canvas.optInt("w", canvas.optInt("width", 240));
                    ch = canvas.optInt("h", canvas.optInt("height", 240));
                }
                JSONArray layersArr = m.optJSONArray("layers");
                if (layersArr != null) {
                    for (int i = 0; i < layersArr.length(); i++) {
                        JSONObject o = layersArr.optJSONObject(i);
                        if (o == null) continue;
                        Layer l = new Layer();
                        l.id = o.optString("id");
                        l.x = o.optInt("x");
                        l.y = o.optInt("y");
                        l.z = o.optInt("z", i);
                        JSONArray fr = o.optJSONArray("frames");
                        if (fr != null) {
                            for (int j = 0; j < fr.length(); j++) {
                                String f = fr.optString(j);
                                if (!f.isEmpty()) l.frames.add(f);
                            }
                        }
                        String single = o.optString("file");
                        if (!single.isEmpty() && l.frames.isEmpty()) l.frames.add(single);
                        JSONObject bl = o.optJSONObject("blink");
                        if (bl != null) {
                            l.blink = true;
                            l.blinkPeriod = bl.optLong("period_ms", 4000);
                            l.blinkClosed = bl.optLong("closed_ms", 120);
                        }
                        l.talk = o.optBoolean("talk");
                        l.alert = o.optBoolean("alert");
                        if (!l.frames.isEmpty()) ly.add(l);
                    }
                    JSONObject emotionsObj = m.optJSONObject("emotions");
                    if (emotionsObj != null) {
                        Iterator<String> states = emotionsObj.keys();
                        while (states.hasNext()) {
                            String state = states.next();
                            JSONObject per = emotionsObj.optJSONObject(state);
                            if (per == null) continue;
                            Map<String, Sel> sel = new HashMap<>();
                            Iterator<String> ids = per.keys();
                            while (ids.hasNext()) {
                                String layerId = ids.next();
                                Object v = per.get(layerId);
                                Sel s = new Sel();
                                if ("cycle".equals(v)) {
                                    s.cycle = true;
                                } else if (v instanceof Number) {
                                    s.frame = ((Number) v).intValue();
                                } else continue;
                                sel.put(layerId, s);
                            }
                            emo.put(state, sel);
                        }
                    }
                }
                // 行动系统：actions（帧轨 / GIF）+ commands 命令映射。
                Map<String, Action> act = new HashMap<>();
                List<String> gifFiles = new ArrayList<>();
                JSONObject actionsObj = m.optJSONObject("actions");
                if (actionsObj != null) {
                    Iterator<String> an = actionsObj.keys();
                    while (an.hasNext()) {
                        String name = an.next();
                        JSONObject o = actionsObj.optJSONObject(name);
                        if (o == null) continue;
                        Action a = new Action();
                        if (!o.optString("gif").isEmpty()) {
                            a.gif = o.optString("gif");
                            a.gifMs = Math.min(10000, Math.max(100, o.optLong("ms", 3000)));
                            gifFiles.add(a.gif);
                        } else {
                            a.frameMs = Math.min(2000, Math.max(30, o.optLong("frame_ms", 120)));
                            a.loops = Math.min(10, Math.max(1, o.optInt("loops", 1)));
                            JSONObject tracks = o.optJSONObject("tracks");
                            if (tracks != null) {
                                Iterator<String> tl = tracks.keys();
                                while (tl.hasNext()) {
                                    String layerId = tl.next();
                                    JSONArray seq = tracks.optJSONArray(layerId);
                                    if (seq == null || seq.length() == 0
                                            || seq.length() > 32) continue;
                                    List<Integer> steps = new ArrayList<>();
                                    for (int i = 0; i < seq.length(); i++) {
                                        steps.add(seq.optInt(i));
                                    }
                                    a.tracks.put(layerId, steps);
                                }
                            }
                            if (a.tracks.isEmpty()) continue;
                        }
                        act.put(name, a);
                    }
                }
                Map<String, String> cmds = new HashMap<>();
                JSONObject commandsObj = m.optJSONObject("commands");
                if (commandsObj != null) {
                    Iterator<String> cn = commandsObj.keys();
                    while (cn.hasNext()) {
                        String slot = cn.next();
                        String target = commandsObj.optString(slot);
                        if (!slot.matches("A[1-9]") || !act.containsKey(target)) continue;
                        cmds.put(slot, target);
                    }
                }
                // 动效参数。
                long bp = 3000; float ba = 0.04f; long sp = 1800; float sa = 7, bo = 8;
                JSONObject motion = m.optJSONObject("motion");
                if (motion != null) {
                    JSONObject breath = motion.optJSONObject("breath");
                    if (breath != null) {
                        int pct = breath.optInt("scale_pct", 108);
                        if (pct >= 100 && pct <= 120) ba = (pct - 100) / 200f;
                        long p = breath.optLong("period_ms", 3000);
                        if (p >= 500 && p <= 10000) bp = p;
                    }
                    int swayDeg = motion.optInt("sway_deg", -1);
                    if (swayDeg >= 0 && swayDeg <= 15) sa = 2 + swayDeg;
                    long swayP = motion.optLong("sway_period_ms", 0);
                    if (swayP >= 200 && swayP <= 10000) sp = swayP;
                }
                // 预下载全部引用文件（去重；GIF 动作文件一并）。
                List<String> wanted = new ArrayList<>();
                for (List<String> fs : frames.values()) {
                    for (String f : fs) if (!wanted.contains(f)) wanted.add(f);
                }
                for (Layer l : ly) {
                    for (String f : l.frames) if (!wanted.contains(f)) wanted.add(f);
                }
                for (String f : gifFiles) if (!wanted.contains(f)) wanted.add(f);
                for (String f : wanted) ensureFile(f);
                final boolean useLayers = !ly.isEmpty() && framesObj == null;
                final int fCw = cw, fCh = ch;
                final long fDef = defMs, fBlinkP = blinkPeriod, fBlinkH = blinkHold;
                final long fBp = bp;
                final float fBa = ba;
                final long fSp = sp;
                final float fSa = sa, fBo = bo;
                ui.post(() -> {
                    statusFrames.clear();
                    statusFrames.putAll(frames);
                    frameInterval.clear();
                    frameInterval.putAll(intervals);
                    layers.clear();
                    layers.addAll(ly);
                    emotions.clear();
                    emotions.putAll(emo);
                    actions.clear();
                    actions.putAll(act);
                    commandMap.clear();
                    commandMap.putAll(cmds);
                    layeredMode = useLayers;
                    canvasW = fCw;
                    canvasH = fCh;
                    defaultFrameMs = fDef;
                    blinkPeriodMs = fBlinkP;
                    blinkHoldMs = fBlinkH;
                    breathPeriod = fBp;
                    breathAmp = fBa;
                    swayPeriod = fSp;
                    swayAmp = fSa;
                    bounceAmp = fBo;
                    activeAction = "";
                    loading = false;
                    frameIndex = 0;
                    frameStart = System.currentTimeMillis();
                    motionStart = System.currentTimeMillis();
                    invalidate();
                    announceActions();
                });
            } catch (Exception e) {
                Log.w(TAG, "manifest load", e);
                loading = false;
            }
        }, "made-sprite").start();
    }

    /** 桥接器素材接口按 ≤4KB 分片（X-Sprite-Total 给全长），循环续拉。 */
    private void ensureFile(String name) {
        try {
            File out = new File(cacheDir(), name);
            if (out.exists() && out.length() > 0) return;
            File tmp = new File(cacheDir(), name + ".tmp");
            FileOutputStream fos = new FileOutputStream(tmp);
            int offset = 0;
            while (true) {
                Bridge.Resp r = bridge.get("/device/sprite/" + provider + "/" + name
                        + "?offset=" + offset + "&len=4096");
                if (!r.ok() || r.bytes.length == 0) break;
                fos.write(r.bytes);
                offset += r.bytes.length;
                long total = -1;
                if (r.headers != null && r.headers.get("X-Sprite-Total") != null) {
                    try {
                        total = Long.parseLong(r.headers.get("X-Sprite-Total").get(0).trim());
                    } catch (NumberFormatException ignored) { }
                }
                if (total <= 0 || offset >= total) break;
            }
            fos.close();
            if (offset > 0 && !tmp.renameTo(out)) {
                Log.w(TAG, "rename failed " + name);
                tmp.delete();
            } else if (offset == 0) {
                tmp.delete();
            }
        } catch (Exception e) {
            Log.w(TAG, "fetch " + name, e);
        }
    }

    private File cacheDir() {
        File d = new File(getContext().getCacheDir(), "sprite/" + provider);
        if (!d.exists()) d.mkdirs();
        return d;
    }

    private Bitmap bitmapFor(String name) {
        if (name == null || name.isEmpty()) return null;
        Bitmap bm = bitmaps.get(name);
        if (bm != null) return bm;
        try {
            File f = new File(cacheDir(), name);
            if (!f.exists()) return null;
            FileInputStream in = new FileInputStream(f);
            bm = BitmapFactory.decodeStream(in);
            in.close();
            if (bm != null) bitmaps.put(name, bm);
            return bm;
        } catch (Exception e) {
            return null;
        }
    }

    private Movie movieFor(String name) {
        if (name == null || name.isEmpty()) return null;
        Movie mv = movies.get(name);
        if (mv != null) return mv;
        try {
            File f = new File(cacheDir(), name);
            if (!f.exists()) return null;
            // Movie.decodeStream 对不可 mark 的流不稳，先整读再解。
            java.io.ByteArrayInputStream in =
                    new java.io.ByteArrayInputStream(readAll(f));
            mv = Movie.decodeStream(in);
            in.close();
            if (mv != null) movies.put(name, mv);
            return mv;
        } catch (Exception e) {
            return null;
        }
    }

    private static byte[] readAll(File f) throws Exception {
        FileInputStream in = new FileInputStream(f);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        in.close();
        return out.toByteArray();
    }

    // ---------- 动效（对齐固件 artUpdate） ----------

    private static float wave(long elapsedMs, long periodMs, float amp, double phaseRad) {
        if (periodMs <= 0) return 0;
        double angle = 2 * Math.PI * (elapsedMs % periodMs) / periodMs + phaseRad;
        return (float) (Math.sin(angle) * amp);
    }

    /** 呼吸/摆动/弹跳随情绪微调（固件 artUpdate 同表）。 */
    private void motionParams(String mood, long[] swayP, float[] swayA,
                              float[] bounceA) {
        swayP[0] = swayPeriod;
        swayA[0] = swayAmp;
        bounceA[0] = bounceAmp;
        switch (mood) {
            case "talking":
                swayP[0] = swayPeriod * 2 / 3;
                swayA[0] += 2;
                bounceA[0] += 2;
                break;
            case "running":
                swayP[0] = swayPeriod * 3 / 4;
                break;
            case "waiting":
                swayP[0] = swayPeriod * 6 / 5;
                swayA[0] -= 2;
                break;
            case "success":
                swayP[0] = swayPeriod * 5 / 6;
                bounceA[0] += 3;
                break;
            case "sad":
                swayP[0] = swayPeriod * 7 / 6;
                swayA[0] -= 2;
                break;
            default:
                break;
        }
    }

    /** 帧轨动作对某层的帧覆盖；不引用返回 -1。 */
    private int actionFrameOverride(Layer l, long actionElapsed) {
        if (activeAction.isEmpty()) return -1;
        Action a = actions.get(activeAction);
        if (a == null || a.gif != null && !a.gif.isEmpty()) return -1;
        List<Integer> track = a.tracks.get(l.id);
        if (track == null || track.isEmpty()) return -1;
        int step = (int) ((actionElapsed / a.frameMs) % (a.loops * track.size()));
        return track.get(step % track.size());
    }

    private String layerFrame(Layer l, long elapsed) {
        int count = l.frames.size();
        Sel sel = emotionSel(l);
        int frame;
        if (l.blink) {
            int open = sel != null && sel.frame >= 0 ? sel.frame : 0;
            int closed = count > 1 ? 1 : open;
            boolean idleLike = status.equals("idle") || status.equals("offline");
            if (sel != null && sel.frame >= 0 && !idleLike) {
                frame = open; // 非待机情绪固定（睡觉/惊讶不眨眼）
            } else if (open != closed && (elapsed % l.blinkPeriod) < l.blinkClosed) {
                frame = closed;
            } else {
                frame = open;
            }
        } else if (l.talk && (status.equals("talking") || status.equals("running"))) {
            if (sel != null && sel.frame >= 0 && !sel.cycle) {
                frame = sel.frame;
            } else {
                frame = (int) ((elapsed / CYCLE_MS) % count);
            }
        } else if (sel != null && sel.frame >= 0) {
            frame = sel.frame;
        } else if (sel != null && sel.cycle) {
            frame = (int) ((elapsed / CYCLE_MS) % count);
        } else {
            frame = 0;
        }
        return l.frames.get(Math.max(0, Math.min(frame, count - 1)));
    }

    private Sel emotionSel(Layer l) {
        Map<String, Sel> per = emotions.get(status);
        return per == null ? null : per.get(l.id);
    }

    // ---------- 绘制 ----------

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int w = getWidth(), h = getHeight();
        long elapsed = System.currentTimeMillis() - frameStart;
        long motionMs = System.currentTimeMillis() - motionStart;

        // 动作时长检查（GIF：ms；帧轨：loops×轨长×frame_ms）。过期自动归位。
        if (!activeAction.isEmpty()) {
            Action a = actions.get(activeAction);
            boolean expired = true;
            if (a != null) {
                long actionElapsed = System.currentTimeMillis() - actionStartMs;
                if (!a.gif.isEmpty()) {
                    expired = actionElapsed >= a.gifMs;
                } else {
                    int trackLen = 1;
                    for (List<Integer> t : a.tracks.values()) {
                        trackLen = Math.max(trackLen, t.size());
                    }
                    expired = actionElapsed >= a.loops * trackLen * a.frameMs;
                }
            }
            if (expired) activeAction = "";
        }

        // 身后光圈脉冲（waiting 提醒，固件 kWaitingConfirm 语义）。
        if (status.equals("waiting") && activeAction.isEmpty()) {
            float baseR = Math.min(w, h) * 0.32f;
            float ph = (motionMs % 1400) / 1400f;
            float r = baseR * (0.92f + 0.16f * ph);
            haloPaint.setStyle(Paint.Style.STROKE);
            haloPaint.setStrokeWidth(Math.min(w, h) * 0.018f);
            haloPaint.setColor(Ui.WARN);
            haloPaint.setAlpha((int) (120 * (1 - ph)));
            canvas.drawCircle(w / 2f, h / 2f, r, haloPaint);
            haloPaint.setAlpha(255);
        }

        // 呼吸 + 轻摆/弹跳（支点=舞台中心；GIF 动作同样随容器动）。
        float unit = Math.min(w, h) * 0.92f / 240f; // 设计 px → 视图 px
        long[] swayP = new long[1];
        float[] swayA = new float[1], bounceA = new float[1];
        String mood = activeAction.isEmpty() ? status : "running";
        motionParams(mood, swayP, swayA, bounceA);
        float dx = wave(motionMs, swayP[0], swayA[0], 0) * unit;
        float bounce = (wave(motionMs, swayP[0] / 2, bounceA[0], Math.PI * 1.5)
                + bounceA[0]) / 2f;
        float dy = -bounce * unit;
        float breath = 1 + breathAmp * (float) Math.sin(
                2 * Math.PI * (motionMs % breathPeriod) / (double) breathPeriod);

        canvas.save();
        canvas.translate(w / 2f + dx, h / 2f + dy);
        canvas.scale(breath, breath);
        canvas.translate(-w / 2f, -h / 2f);

        Action playing = activeAction.isEmpty() ? null : actions.get(activeAction);
        if (playing != null && !playing.gif.isEmpty()) {
            drawGifAction(canvas, w, h, playing);
            canvas.restore();
            return;
        }

        if (layeredMode && !layers.isEmpty()) {
            // 等比缩放到视图，画布按 sprite 坐标系绘制。
            float scale = Math.min(w / (float) canvasW, h / (float) canvasH) * 0.92f;
            float ox = (w - canvasW * scale) / 2f, oy = (h - canvasH * scale) / 2f;
            canvas.save();
            canvas.translate(ox, oy);
            canvas.scale(scale, scale);
            List<Layer> sorted = new ArrayList<>(layers);
            java.util.Collections.sort(sorted, (a, b) -> Integer.compare(a.z, b.z));
            long actionElapsed = System.currentTimeMillis() - actionStartMs;
            for (Layer l : sorted) {
                int override = actionFrameOverride(l, actionElapsed);
                String file = override >= 0 ? l.frames.get(
                        Math.min(override, l.frames.size() - 1)) : layerFrame(l, elapsed);
                Bitmap bm = bitmapFor(file);
                if (bm != null) canvas.drawBitmap(bm, l.x, l.y, paint);
            }
            canvas.restore();
            canvas.restore();
            return;
        }

        List<String> base = currentFrames();
        Bitmap frame = null;
        if (blinkPeriodMs > 0 && !base.isEmpty()) {
            long phase = System.currentTimeMillis() % blinkPeriodMs;
            if (phase >= blinkPeriodMs - blinkHoldMs) {
                List<String> blink = statusFrames.get("blink");
                if (blink != null && !blink.isEmpty()) frame = bitmapFor(blink.get(0));
            }
        }
        if (frame == null && !base.isEmpty()) {
            frame = bitmapFor(base.get(frameIndex % base.size()));
        }
        if (frame != null) {
            float scale = Math.min(w / (float) frame.getWidth(), h / (float) frame.getHeight()) * 0.92f;
            float dw = frame.getWidth() * scale, dh = frame.getHeight() * scale;
            RectF dst = new RectF((w - dw) / 2, (h - dh) / 2, (w + dw) / 2, (h + dh) / 2);
            canvas.drawBitmap(frame, null, dst, paint);
        } else if (!loading) {
            // 素材加载中不画占位鬼影（分页切换时邻页短暂空白比鬼影闪烁好）
            drawFallback(canvas, w, h);
        }
        canvas.restore();
    }

    private void drawGifAction(Canvas canvas, int w, int h, Action a) {
        Movie mv = movieFor(a.gif);
        if (mv == null) return; // 文件缺失/解码失败：静默降级，本帧不画
        long actionElapsed = System.currentTimeMillis() - actionStartMs;
        int dur = mv.duration();
        if (dur <= 0) dur = 1000;
        mv.setTime((int) (actionElapsed % dur));
        float scale = Math.min(w / (float) mv.width(), h / (float) mv.height()) * 0.92f;
        float ox = (w - mv.width() * scale) / 2f, oy = (h - mv.height() * scale) / 2f;
        canvas.save();
        canvas.translate(ox, oy);
        canvas.scale(scale, scale);
        mv.draw(canvas, 0, 0, paint);
        canvas.restore();
    }

    private void drawFallback(Canvas canvas, int w, int h) {
        paint.setColor(0xFFFF6C3E);
        paint.setStyle(Paint.Style.FILL);
        float r = Math.min(w, h) * 0.18f;
        canvas.drawCircle(w / 2f, h / 2f - r * 0.4f, r, paint);
        paint.setColor(0xFFFFFFFF);
        canvas.drawCircle(w / 2f - r * 0.35f, h / 2f - r * 0.6f, r * 0.12f, paint);
        canvas.drawCircle(w / 2f + r * 0.35f, h / 2f - r * 0.6f, r * 0.12f, paint);
        textPaint.setTextSize(sp(14));
        canvas.drawText(label, w / 2f, h / 2f + r * 1.8f, textPaint);
    }

    private float sp(float v) {
        return v * getResources().getDisplayMetrics().scaledDensity;
    }
}
