package com.made.app;

import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;

import javax.net.ssl.HttpsURLConnection;

/**
 * 桥接器 HTTP 层。协议与 ESP32 固件 vibe_app_transport.cpp 完全一致：
 * - Authorization: Bearer &lt;deviceToken&gt;
 * - ngrok-skip-browser-warning: 1（公网穿透场景）
 * - 错误应答统一为 JSON {"error": "..."}
 */
public final class Bridge {
    private static final String TAG = "made.bridge";

    /** 单次 HTTP 交互结果。 */
    public static final class Resp {
        public int code;
        public String body = "";
        public byte[] bytes = new byte[0];
        public Map<String, List<String>> headers;

        public boolean ok() { return code >= 200 && code < 300; }

        /** 从 {"error": "..."} 提取桥接器文案，失败时回退到状态码描述。 */
        public String errorText() {
            try {
                org.json.JSONObject o = new org.json.JSONObject(body);
                String e = o.optString("error", "");
                if (!e.isEmpty()) return e;
            } catch (Exception ignored) { }
            return "HTTP " + code;
        }
    }

    private volatile String baseUrl = "";
    private volatile String token = "";

    public String baseUrl() { return baseUrl; }
    public String token() { return token; }

    public void setEndpoint(String url, String token) {
        while (url.endsWith("/")) url = url.substring(0, url.length() - 1);
        this.baseUrl = url;
        this.token = token == null ? "" : token;
    }

    public boolean isPaired() { return !baseUrl.isEmpty() && !token.isEmpty(); }

    /** 便捷：GET JSON/文本。 */
    public Resp get(String path) { return exchange(path, "GET", null, null, 15000); }

    /** 便捷：POST JSON。 */
    public Resp postJson(String path, String json) {
        return exchange(path, "POST", json == null ? null : json.getBytes(StandardCharsets.UTF_8),
                "application/json", 20000);
    }

    /** 便捷：POST 二进制（如 WAV）。 */
    public Resp postBytes(String path, byte[] data, String contentType, long timeoutMs) {
        return exchange(path, "POST", data, contentType, timeoutMs);
    }

    /** 不带 token 的请求（配对握手用；传完整 URL，因为配对前还没有 baseUrl）。 */
    public Resp exchangeNoAuth(String fullUrl, String method, byte[] body, String contentType) {
        return exchangeInner(fullUrl, method, body, contentType, 10000, false);
    }

    private Resp exchange(String path, String method, byte[] body, String contentType, long timeoutMs) {
        if (!isPaired()) return err("未配对");
        return exchangeInner(baseUrl + path, method, body, contentType, timeoutMs, true);
    }

    private static Resp err(String msg) {
        Resp r = new Resp();
        r.code = 0;
        r.body = "{\"error\":\"" + msg + "\"}";
        return r;
    }

    private Resp exchangeInner(String url, String method, byte[] body, String contentType,
                               long timeoutMs, boolean withAuth) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            if (url.startsWith("https://")) {
                // 局域网桥接器通常是自签或纯 HTTP；HTTPS 场景走系统信任链。
                ((HttpsURLConnection) conn).setHostnameVerifier((h, s) -> true);
            }
            conn.setConnectTimeout(8000);
            conn.setReadTimeout((int) timeoutMs);
            conn.setRequestMethod(method);
            conn.setRequestProperty("ngrok-skip-browser-warning", "1");
            if (withAuth) conn.setRequestProperty("Authorization", "Bearer " + token);
            boolean hasBody = body != null && body.length > 0;
            if (hasBody) {
                conn.setDoOutput(true);
                conn.setRequestProperty("Content-Type",
                        contentType == null ? "application/json" : contentType);
                try (OutputStream os = conn.getOutputStream()) {
                    os.write(body);
                }
            }
            Resp r = new Resp();
            r.code = conn.getResponseCode();
            r.headers = conn.getHeaderFields();
            InputStream in = r.code >= 400 ? conn.getErrorStream() : conn.getInputStream();
            // 文本接口读 body；二进制接口（素材）从 headers/content 里自己取。
            r.bytes = readAll(in);
            r.body = new String(r.bytes, StandardCharsets.UTF_8);
            return r;
        } catch (Exception e) {
            Log.w(TAG, "exchange " + method + " " + url + " -> " + e);
            return err("电脑连接失败");
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    private static byte[] readAll(InputStream in) throws Exception {
        if (in == null) return new byte[0];
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        in.close();
        return out.toByteArray();
    }
}
