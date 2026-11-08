package com.made.app;

import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Locale;
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

    /** 云端滑续：响应头捎带新 cloud JWT 时回调（实现方负责落盘）。任意线程。 */
    public interface CloudTokenListener {
        void onCloudTokenRenewed(String token);
    }

    private volatile CloudTokenListener cloudTokenListener;

    public void setCloudTokenListener(CloudTokenListener l) { cloudTokenListener = l; }

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
    // 云端码得：cloud 颁发的 JWT，所有请求额外携带（与桥接器自身的 Bearer 互不干扰）
    private volatile String cloudToken = "";

    public String baseUrl() { return baseUrl; }
    public String token() { return token; }

    public void setCloudToken(String t) { cloudToken = t == null ? "" : t; }
    public String cloudToken() { return cloudToken; }

    /** cloud JWT 的过期时刻（ms）；解析失败返回 0。App 用它判断临期主动刷新。 */
    public long cloudTokenExpiresAtMs() {
        try {
            String[] parts = cloudToken.split("\\.");
            if (parts.length != 3) return 0;
            byte[] json = android.util.Base64.decode(parts[1],
                    android.util.Base64.URL_SAFE | android.util.Base64.NO_WRAP | android.util.Base64.NO_PADDING);
            return new org.json.JSONObject(new String(json, StandardCharsets.UTF_8)).optLong("exp", 0) * 1000L;
        } catch (Exception ignored) {
            return 0;
        }
    }

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
        return exchangeNoAuth(fullUrl, method, body, contentType, 10000);
    }

    /** 同上，可指定读超时（沙箱冷启动等慢请求用）。 */
    public Resp exchangeNoAuth(String fullUrl, String method, byte[] body, String contentType, long timeoutMs) {
        return exchangeInner(fullUrl, method, body, contentType, timeoutMs, false);
    }

    private Resp exchange(String path, String method, byte[] body, String contentType, long timeoutMs) {
        if (!isPaired()) return err("未配对");
        // 云端路径（经 cloud 转发到沙箱桥接器）单跳延迟远高于局域网：读超时一律放宽到 ≥30s，
        // 否则弱网下素材分片/会话请求频繁超时报「电脑连接失败」。
        if (baseUrl.contains("/bridge/") && timeoutMs < 30000) timeoutMs = 30000;
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
                // 局域网桥接器可能是「证书有效但主机名对不上」（按 IP 访问）的情况，
                // 只对私有网段放行主机名校验；公网/云端域名一律走系统校验，防中间人。
                if (isPrivateHost(conn.getURL().getHost())) {
                    ((HttpsURLConnection) conn).setHostnameVerifier((h, s) -> true);
                }
            }
            // 连接超时 15s：公网链路（Cloudflare/跨境）TLS 握手常超 8s，太短会把弱连接直接判死。
            conn.setConnectTimeout(15000);
            conn.setReadTimeout((int) timeoutMs);
            conn.setRequestMethod(method);
            conn.setRequestProperty("ngrok-skip-browser-warning", "1");
            if (!cloudToken.isEmpty()) conn.setRequestProperty("x-made-cloud-token", cloudToken);
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
            // 云端滑续：服务端在令牌过半衰期时随任意响应捎带新 JWT，捕获并通知落盘。
            // 没有这套机制，7 天 TTL 一到所有用户同时静默 401（2026-11-08 实测）。
            String renewed = null;
            if (r.headers != null) {
                // HttpURLConnection 头字段大小写不敏感，但 Map 键不是：稳妥扫一遍
                for (String k : r.headers.keySet()) {
                    if (k != null && k.equalsIgnoreCase("x-made-cloud-token")) {
                        java.util.List<String> vals = r.headers.get(k);
                        renewed = vals == null || vals.isEmpty() ? null : vals.get(0);
                        break;
                    }
                }
            }
            if (renewed != null && !renewed.isEmpty() && !renewed.equals(cloudToken)) {
                cloudToken = renewed;
                CloudTokenListener l = cloudTokenListener;
                if (l != null) l.onCloudTokenRenewed(renewed);
                Log.i(TAG, "cloud token renewed via response header");
            }
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
        while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
        in.close();
        return out.toByteArray();
    }

    /** 本机 / 局域网私有地址：只有这些才允许绕过 HTTPS 主机名校验。 */
    static boolean isPrivateHost(String host) {
        if (host == null) return false;
        String h = host.toLowerCase(Locale.ROOT);
        if (h.startsWith("[") && h.endsWith("]")) h = h.substring(1, h.length() - 1);
        if (h.isEmpty()) return false;
        if (h.equals("localhost") || h.endsWith(".local") || h.endsWith(".lan")) return true;
        if (h.indexOf(':') >= 0) {
            // IPv6：回环 / 唯一本地 / 链路本地
            return h.equals("::1") || h.startsWith("fc") || h.startsWith("fd")
                    || h.startsWith("fe80");
        }
        if (!h.matches("\\d{1,3}(\\.\\d{1,3}){3}")) return false;
        String[] p = h.split("\\.");
        int a = Integer.parseInt(p[0]);
        int b = Integer.parseInt(p[1]);
        if (a == 10 || a == 127) return true;
        if (a == 192 && b == 168) return true;
        if (a == 172 && b >= 16 && b <= 31) return true;
        return a == 169 && b == 254;
    }
}
