package com.made.app;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.os.Bundle;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

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
 */
public final class H5Activity extends Activity {

    private WebView web;
    private TextView status;
    private String base, deviceToken, cloudToken, dir, name;

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
            if (r.ok()) {
                try { h5Token = new JSONObject(r.body).optString("token"); } catch (Exception ignore) { }
            }
            final String tokenFinal = h5Token;
            final String tokenError = r.ok() || !TextUtils.isEmpty(h5Token) ? "" : r.errorText();
            runOnUiThread(() -> {
                if (TextUtils.isEmpty(tokenFinal)) {
                    status.setText("打开失败：" + (TextUtils.isEmpty(tokenError) ? "拿不到插件令牌" : tokenError));
                    return;
                }
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

    // ================= HTTP（同 MarketActivity：云端 + 设备双重鉴权） =================

    private Bridge.Resp http(String method, String path, byte[] body) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(base + path).openConnection();
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
