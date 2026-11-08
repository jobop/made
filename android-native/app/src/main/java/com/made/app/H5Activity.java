package com.made.app;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.net.Uri;
import android.os.Bundle;
import android.text.TextUtils;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/**
 * H5 插件容器：加载桥接器提供的插件静态页 /plugins/<dir>/。
 * 打开前用设备令牌换一张短期 scoped token（能力协商：页面能调的接口
 * 以插件 manifest 声明的 capabilities 为限），拼在 URL 上交给页面。
 * 云端沙箱路径走云代理（cloud_token 查询参数鉴权），局域网直连无此参数。
 *
 * 本地缓存（省云端额度）：静态资源统一经 shouldInterceptRequest 接管——
 * 命中本地缓存直接回（零网络流量，沙箱回收了也能玩纯静态游戏）；
 * 未命中在线拉取并落盘，下次打开离线可用。失效策略用 /api/installed
 * 里的 version（market.json 版本）：版本一致沿用缓存，不同/为空则清缓存重下。
 * 数据接口 /h5api/* 不缓存——它需要桥接器在线，离线时页面自己显示未连接。
 */
public final class H5Activity extends Activity {

    private static final String TAG = "made.h5";
    /** 单文件缓存上限：超过只透传不落盘（防异常包撑爆存储）。 */
    private static final long MAX_CACHE_FILE = 8L * 1024 * 1024;

    private WebView web;
    private TextView status;
    private String base, deviceToken, cloudToken, dir, name;
    private File cacheDir;
    private volatile boolean online = false;   // 打开时桥接器是否可达
    private volatile String pendingVersion;    // 在线拉到的新版本；onPageFinished 落盘

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        base = getIntent().getStringExtra("base");
        deviceToken = getIntent().getStringExtra("token");
        cloudToken = getIntent().getStringExtra("cloudToken");
        dir = getIntent().getStringExtra("dir");
        name = getIntent().getStringExtra("name");
        if (TextUtils.isEmpty(base) || TextUtils.isEmpty(deviceToken) || TextUtils.isEmpty(dir)) {
            Toast.makeText(this, "缺少插件信息", Toast.LENGTH_SHORT).show();
            finish();
            return;
        }
        // dir 来自 /api/installed 的包 id（桥接器侧已限 ^[a-z0-9][a-z0-9_-]{0,39}$），仍防御一次
        if (!dir.matches("[a-z0-9][a-z0-9_-]{0,39}")) {
            Toast.makeText(this, "插件标识不合法", Toast.LENGTH_SHORT).show();
            finish();
            return;
        }
        cacheDir = new File(getFilesDir(), "h5cache/" + EndpointScope.cacheKey(base, deviceToken) + "/" + dir);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Ui.BG);

        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setBackgroundColor(Ui.SURFACE);
        bar.setPadding(dp(10), dp(10), dp(18), dp(10));
        TextView back = Ui.text(this, "‹", 26, Ui.INK, false);
        back.setPadding(dp(10), 0, dp(14), 0);
        back.setOnClickListener(v -> goBack());
        bar.addView(back, lp(-2, -2));
        bar.addView(Ui.text(this, TextUtils.isEmpty(name) ? "插件" : name, 17, Ui.INK, true), lp(0, -2, 1f));
        TextView reload = Ui.text(this, "刷新", 14, Ui.PRIMARY, true);
        reload.setPadding(dp(10), dp(6), dp(10), dp(6));
        Ui.press(reload);
        reload.setOnClickListener(v -> open());
        bar.addView(reload, lp(-2, -2));
        root.addView(bar, lp(-1, -2));
        View line = new View(this);
        line.setBackgroundColor(Ui.LINE);
        root.addView(line, lp(-1, dp(1)));

        status = Ui.text(this, "正在打开…", 13, Ui.SUB, false);
        status.setPadding(dp(18), dp(10), dp(18), dp(4));
        root.addView(status, lp(-1, -2));

        web = new WebView(this);
        web.setVisibility(View.GONE);
        setupWeb();
        root.addView(web, new LinearLayout.LayoutParams(-1, 0, 1f));
        setContentView(root);

        open();
    }

    private int dp(float v) { return Ui.dp(this, v); }

    private LinearLayout.LayoutParams lp(int w, int h) { return new LinearLayout.LayoutParams(w, h); }

    private LinearLayout.LayoutParams lp(int w, int h, float weight) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(w, h);
        params.weight = weight;
        return params;
    }

    @SuppressLint("SetJavaScriptEnabled")
    private void setupWeb() {
        web.getSettings().setJavaScriptEnabled(true);
        web.getSettings().setDomStorageEnabled(true);
        web.setWebViewClient(new WebViewClient() {
            @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                return false; // 页面内跳转留在容器里
            }

            @Override public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest req) {
                return intercept(req);
            }

            @Override public void onPageFinished(WebView view, String url) {
                // 版本号在页面加载完成后才落盘：中途退出只留下部分缓存，下次在线会刷新
                String v = pendingVersion;
                if (online && v != null) {
                    pendingVersion = null;
                    writeVersionFile(v);
                }
            }
        });
        web.setWebChromeClient(new WebChromeClient() {
            @Override public void onProgressChanged(WebView view, int newProgress) {
                if (newProgress >= 100 && web.getVisibility() == View.VISIBLE) status.setText("");
            }
        });
    }

    /** 换 scoped token → 加载插件页（失败显示原因，可点刷新重试）。 */
    private void open() {
        status.setText("正在打开…");
        web.setVisibility(View.GONE);
        new Thread(() -> {
            String h5Token = "";
            Bridge.Resp r = http("GET", "/plugins/" + dir + "/token", null);
            final boolean reachable = r.ok();
            if (reachable) {
                try { h5Token = new JSONObject(r.body).optString("token"); } catch (Exception ignore) { }
            }
            // 版本检查（仅在线时）：/api/installed 的 h5apps[].version（market.json，可为空）
            String version = "";
            if (reachable) {
                Bridge.Resp inst = http("GET", "/api/installed", null);
                if (inst.ok()) {
                    try {
                        JSONArray apps = new JSONObject(inst.body).optJSONArray("h5apps");
                        for (int i = 0; apps != null && i < apps.length(); i++) {
                            JSONObject a = apps.optJSONObject(i);
                            if (a != null && dir.equals(a.optString("id"))) {
                                version = a.optString("version");
                                break;
                            }
                        }
                    } catch (Exception ignore) { }
                }
            }
            final boolean hasCache = new File(cacheDir, "index.html").isFile();
            final String cachedVer = readVersionFile();
            if (reachable) {
                boolean reusable = hasCache && !version.isEmpty() && version.equals(cachedVer);
                if (!reusable) clearCacheDir(); // 版本变化/无版本号/首次：清掉重下（本次在线边拉边缓存）
                if (!version.isEmpty()) pendingVersion = version;
            }
            final boolean offlineOk = !reachable && hasCache; // 离线且无缓存：真的打不开
            final String tokenFinal = h5Token;
            final String tokenError = r.errorText();
            runOnUiThread(() -> {
                if (offlineOk) {
                    status.setText("离线模式：正在使用本地缓存（数据类功能需桥接器在线）");
                } else if (!reachable) {
                    status.setText("打开失败：桥接器不可达（" + tokenError
                            + "）。缓存过的应用可离线使用，这个应用还没有缓存");
                    return;
                } else {
                    status.setText("正在打开…");
                }
                online = reachable;
                StringBuilder url = new StringBuilder(base)
                        .append("/plugins/").append(dir).append("/?h5token=").append(tokenFinal);
                if (!TextUtils.isEmpty(cloudToken)) url.append("&cloud_token=").append(cloudToken);
                web.loadUrl(url.toString());
                web.setVisibility(View.VISIBLE);
            });
        }, "made.h5").start();
    }

    private void goBack() {
        if (web != null && web.canGoBack()) web.goBack(); else finish();
    }

    @Override public void onBackPressed() {
        goBack();
    }

    @Override protected void onDestroy() {
        if (web != null) web.destroy();
        super.onDestroy();
    }

    // ================= 本地缓存 =================

    /**
     * 静态资源拦截：URL 路径含 /plugins/<dir>/ 即视为本插件资源（局域网与云端
     * /bridge/<uid>/plugins/ 两种前缀都能命中）。缓存命中回本地；未命中且在线
     * 则拉网络、落盘再回；离线未命中回 404。/h5api/* 与其他路径放行走网络。
     */
    private WebResourceResponse intercept(WebResourceRequest req) {
        try {
            Uri u = req.getUrl();
            String root = base + "/plugins/" + dir;
            if (!"GET".equals(req.getMethod()) || !EndpointScope.belongsTo(u.toString(), root)) return null;
            String prefix = Uri.parse(root).getPath() + "/";
            String path = u.getPath();
            if (path == null || !path.startsWith(prefix)) return null;
            String rel = path.substring(prefix.length());
            if (rel.equals("token")) return null; // token 接口走原逻辑（scoped token JSON，不缓存）
            // 主页也纳入缓存：单文件 H5 游戏（全部内联在一个 index.html 里）只有这一个
            // 请求，放过主页 = 什么都不缓存。主页和其他静态资源同策略：命中回本地。
            if (rel.isEmpty()) rel = "index.html";
            if (rel.endsWith("/")) rel = rel + "index.html";
            if (rel.contains("..")) return notFound(); // 防路径穿越
            File f = new File(cacheDir, rel);
            if (!f.getCanonicalPath().startsWith(cacheDir.getCanonicalPath() + File.separator)) return notFound();
            if (f.isFile() && f.length() > 0) return fileResponse(f);
            if (!online) return notFound();
            byte[] data = fetchStatic(rel);
            if (data == null) return notFound();
            if (data.length <= MAX_CACHE_FILE) writeCacheFile(rel, data);
            return bytesResponse(data, mimeOf(rel));
        } catch (Exception e) {
            Log.w(TAG, "intercept", e);
            return null; // 放行走网络，别把页面弄挂
        }
    }

    private WebResourceResponse fileResponse(File f) {
        try {
            return new WebResourceResponse(mimeOf(f.getName()), charsetOf(f.getName()),
                    new FileInputStream(f));
        } catch (Exception e) {
            return null;
        }
    }

    private WebResourceResponse bytesResponse(byte[] data, String mime) {
        return new WebResourceResponse(mime, charsetOf(mime), new ByteArrayInputStream(data));
    }

    private WebResourceResponse notFound() {
        return new WebResourceResponse("text/plain", "utf-8", 404, "Not Found",
                null, new ByteArrayInputStream(new byte[0]));
    }

    /** 在线拉一个静态资源（带云端令牌头；静态资源本身免设备令牌）。失败返回 null。 */
    private byte[] fetchStatic(String rel) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(base + "/plugins/" + dir + "/" + rel).openConnection();
            conn.setInstanceFollowRedirects(false);
            conn.setConnectTimeout(15000);
            conn.setReadTimeout(20000);
            conn.setRequestMethod("GET");
            if (!TextUtils.isEmpty(cloudToken)) {
                conn.setRequestProperty("x-made-cloud-token", cloudToken);
            }
            int code = conn.getResponseCode();
            if (code < 200 || code >= 300) return null;
            long len = conn.getContentLength();
            if (len > MAX_CACHE_FILE * 4) return null; // 明显异常的大文件直接放弃
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            try (InputStream in = conn.getInputStream()) {
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
            }
            return out.toByteArray();
        } catch (Exception e) {
            return null;
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    private void writeCacheFile(String rel, byte[] data) {
        try {
            File f = new File(cacheDir, rel);
            File parent = f.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) return;
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(f)) {
                out.write(data);
            }
        } catch (Exception e) {
            Log.w(TAG, "cache write " + rel, e);
        }
    }

    private void clearCacheDir() {
        pendingVersion = null;
        File ver = versionFile();
        if (ver.isFile()) //noinspection ResultOfMethodCallIgnored
            ver.delete();
        File[] files = cacheDir.listFiles();
        if (files == null) return;
        for (File f : files) recursiveDelete(f);
    }

    private static void recursiveDelete(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) recursiveDelete(c);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    private File versionFile() {
        return new File(cacheDir.getParentFile(), dir + ".ver");
    }

    private void writeVersionFile(String version) {
        try {
            File ver = versionFile();
            File parent = ver.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) return;
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(ver)) {
                out.write(version.getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception ignored) { }
    }

    private String readVersionFile() {
        try {
            String s = Model.readSmallFile(versionFile());
            return s == null ? "" : s.trim();
        } catch (Exception e) {
            return "";
        }
    }

    private static String mimeOf(String name) {
        String n = name.toLowerCase();
        if (n.endsWith(".html") || n.endsWith(".htm")) return "text/html";
        if (n.endsWith(".js") || n.endsWith(".mjs")) return "text/javascript";
        if (n.endsWith(".css")) return "text/css";
        if (n.endsWith(".json")) return "application/json";
        if (n.endsWith(".png")) return "image/png";
        if (n.endsWith(".jpg") || n.endsWith(".jpeg")) return "image/jpeg";
        if (n.endsWith(".gif")) return "image/gif";
        if (n.endsWith(".svg")) return "image/svg+xml";
        if (n.endsWith(".webp")) return "image/webp";
        if (n.endsWith(".ico")) return "image/x-icon";
        if (n.endsWith(".wasm")) return "application/wasm";
        if (n.endsWith(".txt")) return "text/plain";
        if (n.endsWith(".mp3")) return "audio/mpeg";
        if (n.endsWith(".wav")) return "audio/wav";
        if (n.endsWith(".ogg")) return "audio/ogg";
        if (n.endsWith(".mp4")) return "video/mp4";
        if (n.endsWith(".webm")) return "video/webm";
        if (n.endsWith(".woff")) return "font/woff";
        if (n.endsWith(".woff2")) return "font/woff2";
        if (n.endsWith(".ttf")) return "font/ttf";
        if (n.endsWith(".otf")) return "font/otf";
        if (n.endsWith(".xml")) return "application/xml";
        return "application/octet-stream";
    }

    private static String charsetOf(String mimeOrName) {
        String m = mimeOrName.toLowerCase();
        boolean textual = m.startsWith("text/") || m.contains("json") || m.contains("xml")
                || m.contains("javascript");
        return textual ? "utf-8" : null;
    }

    // ================= HTTP（同 MarketActivity：云端 + 设备双重鉴权） =================

    private Bridge.Resp http(String method, String path, byte[] body) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(base + path).openConnection();
            conn.setInstanceFollowRedirects(false);
            conn.setConnectTimeout(10000);
            conn.setReadTimeout(30000);
            conn.setRequestMethod(method);
            conn.setRequestProperty("x-made-cloud-token", cloudToken == null ? "" : cloudToken);
            conn.setRequestProperty("Authorization", "Bearer " + deviceToken);
            if (body != null && body.length > 0) {
                conn.setDoOutput(true);
                conn.setRequestProperty("Content-Type", "application/json");
                try (OutputStream os = conn.getOutputStream()) {
                    os.write(body);
                }
            }
            Bridge.Resp r = new Bridge.Resp();
            r.code = conn.getResponseCode();
            InputStream in = r.code >= 400 ? conn.getErrorStream() : conn.getInputStream();
            r.bytes = readAll(in);
            r.body = new String(r.bytes, StandardCharsets.UTF_8);
            return r;
        } catch (Exception e) {
            Bridge.Resp r = new Bridge.Resp();
            r.code = 0;
            r.body = "{\"error\":\"网络请求失败\"}";
            return r;
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    private static byte[] readAll(InputStream in) throws Exception {
        if (in == null) return new byte[0];
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
        in.close();
        return out.toByteArray();
    }
}
