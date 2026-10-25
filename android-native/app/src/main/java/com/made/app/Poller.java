package com.made.app;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * 后台轮询：心跳保活 + 配置/会话/任务刷新。节奏与固件 workerLoop 对齐
 * （心跳 ~2.5s，任务/会话随轮）。结果在 UI 线程回调。
 */
public final class Poller {
    private static final String TAG = "made.poller";
    private static final long INTERVAL_MS = 2500;

    public interface Listener {
        /** UI 线程：状态已刷新。 */
        void onModelUpdated();

        /** UI 线程：401，配对被电脑解除。 */
        void onUnpaired();
    }

    /** 板侧命令下沉（action.* 等），UI 线程回调。 */
    public interface CommandSink {
        void onCommand(String id, String name, String target, String direction);
    }

    private CommandSink commandSink;
    private final Set<String> deliveredCommandIds = new HashSet<>();

    public void setCommandSink(CommandSink sink) { commandSink = sink; }

    private final Bridge bridge;
    private final Model model;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final AtomicBoolean running = new AtomicBoolean(false);
    private Thread worker;
    private long bootMs = System.currentTimeMillis();

    public Poller(Bridge bridge, Model model) {
        this.bridge = bridge;
        this.model = model;
    }

    public void start(Listener listener) {
        if (running.getAndSet(true)) return;
        bootMs = System.currentTimeMillis();
        worker = new Thread(() -> loop(listener), "made-poller");
        worker.setDaemon(true);
        worker.start();
    }

    public void stop() {
        running.set(false);
        if (worker != null) worker.interrupt();
    }

    /** 立即触发一轮刷新（发任务/确认后调用）。 */
    public void kick() {
        // worker 的 sleep 被打断后立刻进入下一轮。
        if (worker != null) worker.interrupt();
    }

    private void loop(Listener listener) {
        int cycle = 0;
        while (running.get()) {
            long started = System.currentTimeMillis();
            try {
                if (bridge.isPaired()) {
                    pullCommands();
                    boolean unpaired = refreshOnce(cycle);
                    if (unpaired) {
                        ui.post(listener::onUnpaired);
                        return;
                    }
                    ui.post(listener::onModelUpdated);
                }
            } catch (Exception e) {
                Log.w(TAG, "cycle", e);
                model.connectionError = "电脑连接失败";
            }
            cycle++;
            long elapsed = System.currentTimeMillis() - started;
            try {
                Thread.sleep(Math.max(300, INTERVAL_MS - elapsed));
            } catch (InterruptedException e) {
                // kick 或 stop
            }
        }
    }

    /** 拉取并确认板侧命令（对齐固件 pullBoardCommands：先拉后 ack，去重）。 */
    private void pullCommands() {
        try {
            Bridge.Resp r = bridge.get("/device/commands");
            if (!r.ok()) return;
            JSONArray arr = new JSONObject(r.body).optJSONArray("commands");
            if (arr == null) return;
            List<String> ackIds = new ArrayList<>();
            for (int i = 0; i < arr.length() && i < 8; i++) {
                JSONObject c = arr.optJSONObject(i);
                if (c == null) continue;
                String id = c.optString("id");
                String name = c.optString("name");
                if (id.isEmpty() || id.length() > 32 || name.isEmpty()) continue;
                ackIds.add(id);
                if (!deliveredCommandIds.add(id)) continue;
                if (deliveredCommandIds.size() > 64) deliveredCommandIds.clear();
                final String cid = id, cname = name;
                final String target = c.optString("id"), direction = c.optString("direction");
                ui.post(() -> {
                    if (commandSink != null) commandSink.onCommand(cid, cname, target, direction);
                });
            }
            if (!ackIds.isEmpty()) {
                JSONObject body = new JSONObject();
                body.put("ids", new JSONArray(ackIds));
                bridge.postJson("/device/commands/ack", body.toString());
            }
        } catch (Exception e) {
            Log.w(TAG, "commands", e);
        }
    }

    /** @return true = 401 需要重新配对。 */
    private boolean refreshOnce(int cycle) {
        // 心跳：保持在线（桥接器按最后活跃判在线）。
        bridge.get("/device/heartbeat?upMs=" + (System.currentTimeMillis() - bootMs));

        // 配置：每 2 轮拉一次（含 provider 目录 + 语音可用性）。
        boolean refreshConfig = cycle % 2 == 0 || model.providers.isEmpty();
        if (refreshConfig) {
            Bridge.Resp r = bridge.get("/device/config");
            if (r.code == 401) return true;
            if (r.ok()) {
                try {
                    JSONObject o = new JSONObject(r.body);
                    Model.parseProviders(o.optJSONArray("providers"), model.providers);
                    if (model.selectedProvider >= model.providers.size()
                            || model.selectedProvider < 0) {
                        // 默认选第一个可用的。
                        model.selectedProvider = -1;
                        for (int i = 0; i < model.providers.size(); i++) {
                            if (model.providers.get(i).available) {
                                model.selectedProvider = i;
                                break;
                            }
                        }
                        if (model.selectedProvider < 0 && !model.providers.isEmpty()) {
                            model.selectedProvider = 0;
                        }
                        model.selectedSessionId = ""; // provider 变了，会话重新选
                        model.sessions.clear();
                        model.jobs.clear();
                    }
                    model.voiceAvailable = o.optJSONObject("voice") != null
                            && o.optJSONObject("voice").optBoolean("available");
                    model.defaultProject = o.optString("defaultProject");
                    model.connectionError = "";
                } catch (Exception e) {
                    Log.w(TAG, "config parse", e);
                }
            } else if (r.code > 0) {
                model.connectionError = "配置拉取失败：" + r.errorText();
            }
            return false;
        }

        Model.Provider p = model.provider();
        if (p == null || p.id.isEmpty()) return false;

        // 会话列表（每轮）。
        {
            Bridge.Resp r = bridge.get("/device/sessions?provider=" + p.id);
            if (r.code == 401) return true;
            if (r.ok()) {
                try {
                    Model.parseSessions(new JSONObject(r.body).optJSONArray("sessions"),
                            model.sessions);
                    boolean stillThere = false;
                    for (Model.Session s : model.sessions) {
                        if (s.id.equals(model.selectedSessionId)) { stillThere = true; break; }
                    }
                    if (!stillThere) {
                        model.selectedSessionId = model.sessions.isEmpty()
                                ? "" : model.sessions.get(0).id;
                        model.jobs.clear();
                    }
                    model.connectionError = "";
                } catch (Exception e) {
                    Log.w(TAG, "sessions parse", e);
                }
            }
        }

        // 当前会话任务（每轮）。
        if (!model.selectedSessionId.isEmpty()) {
            Bridge.Resp r = bridge.get("/device/tasks?provider=" + p.id
                    + "&sessionId=" + model.selectedSessionId);
            if (r.code == 401) return true;
            if (r.ok()) {
                try {
                    Model.parseJobs(new JSONObject(r.body).optJSONArray("jobs"), model.jobs);
                    model.connectionError = "";
                } catch (Exception e) {
                    Log.w(TAG, "jobs parse", e);
                }
            }
        }
        return false;
    }
}
