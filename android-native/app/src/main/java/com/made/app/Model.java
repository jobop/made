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
        public List<String> attachments = new ArrayList<>();   // 助手回复的附件（输出）
        public List<String> inputImages = new ArrayList<>();  // 用户本轮发的图片（输入）
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
            String s = readSmallFile(new java.io.File(spriteRootDir(), "catalog.json"));
            return s == null || s.isEmpty() ? null : s;
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * 一次读完整个小文件（catalog / manifest / version.txt 这类）。
     * 必须循环读到 -1：单次 read 不保证读满，长文件会被截断成半个 JSON。
     *
     * @return 空文件返回 ""，文件不存在或过大返回 null
     */
    static String readSmallFile(java.io.File f) throws java.io.IOException {
        if (f == null || !f.exists() || !f.isFile()) return null;
        long len = f.length();
        if (len == 0) return "";
        if (len > 8L * 1024 * 1024) return null;   // 状态文件不该这么大，防异常长度（int 溢出）
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream((int) len);
        try (java.io.FileInputStream in = new java.io.FileInputStream(f)) {
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
        }
        return new String(out.toByteArray(), java.nio.charset.StandardCharsets.UTF_8);
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
            JSONArray input = o.optJSONArray("inputImages");
            if (input != null) {
                for (int k = 0; k < input.length(); k++) {
                    JSONObject a = input.optJSONObject(k);
                    if (a != null && !a.optString("url").isEmpty()) {
                        j.inputImages.add(a.optString("url"));
                    }
                }
            }
            out.add(j);
        }
    }
}
