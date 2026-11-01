package com.made.app;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/** 应用状态镜像（桥接器 /device/config + sessions + tasks 的解析结果）。 */
public final class Model {

    public static final class Provider {
        public String id = "", label = "", reason = "";
        public boolean available;
        public String spriteVersion = "";
        public List<String> spriteFiles = new ArrayList<>();
    }

    public static final class Session {
        public String id = "", title = "", provider = "", projectId = "";
    }

    public static final class Job {
        public String id = "", instruction = "", status = "", progress = "",
                result = "", error = "", sessionId = "";
        public List<String> attachments = new ArrayList<>();
    }

    public final List<Provider> providers = new ArrayList<>();
    public final List<Session> sessions = new ArrayList<>();
    public final List<Job> jobs = new ArrayList<>(); // 当前会话最新一条

    public int selectedProvider = -1;
    public String selectedSessionId = "";
    public boolean voiceAvailable;
    public String defaultProject = "";
    public String connectionError = ""; // 非空 = 链路异常提示

    public Provider provider() {
        return selectedProvider >= 0 && selectedProvider < providers.size()
                ? providers.get(selectedProvider) : null;
    }

    // ---------- 目录持久化（沙箱回收/弱网时的本地兜底） ----------

    /** application context，MainActivity 启动时注入。 */
    public static android.content.Context appContext;

    private static java.io.File spriteRootDir() {
        java.io.File d = new java.io.File(appContext.getFilesDir(), "sprite");
        if (!d.exists()) d.mkdirs();
        return d;
    }

    /** 保存助手目录原文（/device/config 的 providers 数组）：桥接器不可达时兜底。 */
    public static void saveCatalog(String providersJson) {
        if (appContext == null || providersJson == null) return;
        try {
            java.io.FileOutputStream out = new java.io.FileOutputStream(
                    new java.io.File(spriteRootDir(), "catalog.json"));
            out.write(providersJson.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            out.close();
        } catch (Exception ignored) { }
    }

    /** 上次保存的助手目录 JSON；没有返回 null。 */
    public static String loadCatalog() {
        if (appContext == null) return null;
        try {
            java.io.File f = new java.io.File(spriteRootDir(), "catalog.json");
            if (!f.exists()) return null;
            byte[] buf = new byte[(int) f.length()];
            java.io.FileInputStream in = new java.io.FileInputStream(f);
            int n = in.read(buf);
            in.close();
            return n > 0 ? new String(buf, java.nio.charset.StandardCharsets.UTF_8) : null;
        } catch (Exception e) {
            return null;
        }
    }

    public Session session() {
        for (Session s : sessions) if (s.id.equals(selectedSessionId)) return s;
        return null;
    }

    public static void parseProviders(JSONArray arr, List<Provider> out) {
        out.clear();
        if (arr == null) return;
        for (int i = 0; i < arr.length(); i++) {
            JSONObject o = arr.optJSONObject(i);
            if (o == null) continue;
            Provider p = new Provider();
            p.id = o.optString("id");
            p.label = o.optString("label", p.id);
            p.reason = o.optString("reason");
            p.available = o.optBoolean("available");
            JSONObject sprite = o.optJSONObject("sprite");
            if (sprite != null) {
                p.spriteVersion = sprite.optString("version");
                JSONArray files = sprite.optJSONArray("files");
                if (files != null) {
                    for (int j = 0; j < files.length(); j++) {
                        JSONObject f = files.optJSONObject(j);
                        if (f != null) p.spriteFiles.add(f.optString("file"));
                    }
                }
            }
            out.add(p);
        }
    }

    public static void parseSessions(JSONArray arr, List<Session> out) {
        out.clear();
        if (arr == null) return;
        for (int i = 0; i < arr.length(); i++) {
            JSONObject o = arr.optJSONObject(i);
            if (o == null) continue;
            Session s = new Session();
            s.id = o.optString("id");
            s.title = o.optString("title");
            s.provider = o.optString("provider");
            s.projectId = o.optString("projectId");
            out.add(s);
        }
    }

    public static void parseJobs(JSONArray arr, List<Job> out) {
        out.clear();
        if (arr == null) return;
        for (int i = 0; i < arr.length() && out.size() < 8; i++) {
            JSONObject o = arr.optJSONObject(i);
            if (o == null) continue;
            Job j = new Job();
            j.id = o.optString("id");
            j.instruction = o.optString("instruction");
            j.status = o.optString("status");
            j.progress = o.optString("progress");
            j.result = o.optString("result");
            j.error = o.optString("error");
            j.sessionId = o.optString("vibeSessionId");
            JSONArray att = o.optJSONArray("attachments");
            if (att != null) {
                for (int k = 0; k < att.length(); k++) {
                    JSONObject a = att.optJSONObject(k);
                    if (a != null && !a.optString("url").isEmpty()) {
                        j.attachments.add(a.optString("url"));
                    }
                }
            }
            out.add(j);
        }
    }
}
