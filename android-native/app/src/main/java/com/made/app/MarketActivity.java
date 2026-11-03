package com.made.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;
import android.text.TextUtils;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 插件市场：列表来自桥接器 /api/market/index（桥接器代理官方市场，白名单 + 缓存），
 * 安装走 /api/plugins/install（下载 → sha256 校验 → 解包 → 桥接器内 reload 即时生效）。
 * 云端沙箱与局域网/USB 桥接器走同一套 API，页面不做模式区分。
 */
public final class MarketActivity extends Activity {

    /** kind → 分组标题，顺序即页面分组顺序。 */
    private static final String[][] KINDS = {
            {"agent", "助手"},
            {"control", "管控"},
            {"theme", "主题"},
    };

    private String base, token, cloudToken;
    private volatile String vibeToken = "";
    private LinearLayout list;
    private TextView status;
    private final Map<String, List<JSONObject>> groups = new LinkedHashMap<>();

    @Override protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        base = getIntent().getStringExtra("base");
        token = getIntent().getStringExtra("token");
        cloudToken = getIntent().getStringExtra("cloudToken");
        if (TextUtils.isEmpty(base) || TextUtils.isEmpty(token)) {
            Toast.makeText(this, "缺少连接信息", Toast.LENGTH_SHORT).show();
            finish();
            return;
        }
        buildUi();
        reload();
    }

    private int dp(float v) { return Ui.dp(this, v); }

    private void buildUi() {
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
        back.setOnClickListener(v -> finish());
        bar.addView(back, lp(-2, -2));
        bar.addView(Ui.text(this, "插件市场", 17, Ui.INK, true), lp(0, -2, 1f));
        TextView refresh = Ui.text(this, "刷新", 14, Ui.PRIMARY, true);
        refresh.setPadding(dp(10), dp(6), dp(10), dp(6));
        refresh.setOnClickListener(v -> reload());
        Ui.press(refresh);
        bar.addView(refresh, lp(-2, -2));
        root.addView(bar, lp(-1, -2));
        View line = new View(this);
        line.setBackgroundColor(Ui.LINE);
        root.addView(line, lp(-1, dp(1)));

        status = Ui.text(this, "加载中…", 13, Ui.SUB, false);
        status.setPadding(dp(18), dp(12), dp(18), dp(4));
        root.addView(status, lp(-1, -2));

        ScrollView scroll = new ScrollView(this);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setPadding(dp(14), dp(4), dp(14), dp(24));
        scroll.addView(list, lp(-1, -2));
        root.addView(scroll, lp(-1, 0, 1f));
        setContentView(root);
    }

    private LinearLayout.LayoutParams lp(int w, int h) {
        return new LinearLayout.LayoutParams(w, h);
    }

    private LinearLayout.LayoutParams lp(int w, int h, float weight) {
        return new LinearLayout.LayoutParams(w, h, weight);
    }

    private void reload() {
        status.setText("加载中…");
        new Thread(() -> {
            Bridge.Resp s = http("GET", "/api/session", null);
            if (s.ok()) {
                try { vibeToken = new JSONObject(s.body).optString("token"); } catch (Exception ignore) { }
            }
            Bridge.Resp r = http("GET", "/api/market/index", null);
            String error = r.ok() ? "" : r.errorText();
            Map<String, List<JSONObject>> parsed = new LinkedHashMap<>();
            boolean stale = false;
            if (r.ok()) {
                try {
                    JSONObject data = new JSONObject(r.body);
                    stale = data.optBoolean("stale", false);
                    JSONArray arr = data.optJSONArray("packages");
                    if (arr != null) {
                        for (int i = 0; i < arr.length(); i++) {
                            JSONObject p = arr.optJSONObject(i);
                            if (p == null) continue;
                            String kind = p.optString("kind", "");
                            if (!parsed.containsKey(kind)) parsed.put(kind, new ArrayList<>());
                            parsed.get(kind).add(p);
                        }
                    }
                } catch (Exception e) {
                    error = "数据解析失败";
                }
            }
            final String err = error;
            final boolean staleFinal = stale;
            final Map<String, List<JSONObject>> result = parsed;
            runOnUiThread(() -> render(err, staleFinal, result));
        }, "made.market").start();
    }

    private void render(String error, boolean stale, Map<String, List<JSONObject>> parsed) {
        groups.clear();
        groups.putAll(parsed);
        list.removeAllViews();
        if (!TextUtils.isEmpty(error)) {
            status.setText("加载失败：" + error);
            TextView retry = pillButton("重试", Ui.SURFACE, Ui.INK, null);
            retry.setOnClickListener(v -> reload());
            list.addView(retry, lp(-1, -2));
            return;
        }
        int total = 0;
        for (List<JSONObject> items : groups.values()) total += items.size();
        status.setText(total == 0 ? "市场暂时是空的" : stale ? "网络不稳，显示的是最近一次的列表" : "安装后即时生效，助手装完就能用");
        for (String[] kind : KINDS) {
            List<JSONObject> items = groups.get(kind[0]);
            if (items == null || items.isEmpty()) continue;
            TextView head = Ui.text(this, kind[1], 14, Ui.SUB, true);
            head.setPadding(dp(4), dp(14), dp(4), dp(6));
            list.addView(head, lp(-1, -2));
            for (JSONObject p : items) list.addView(packageCard(p), lp(-1, -2));
        }
    }

    private View packageCard(JSONObject p) {
        String name = p.optString("name", p.optString("id", "未命名"));
        String version = p.optString("version", "");
        boolean installed = p.optBoolean("installed", false);

        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(Ui.card(this, Ui.SURFACE, 12, Ui.LINE, 1));
        card.setPadding(dp(14), dp(12), dp(14), dp(12));
        LinearLayout.LayoutParams cardLp = lp(-1, -2);
        cardLp.topMargin = dp(8);

        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout titleCol = new LinearLayout(this);
        titleCol.setOrientation(LinearLayout.VERTICAL);
        titleCol.addView(Ui.text(this, name, 15, Ui.INK, true), lp(-2, -2));
        String sub = "v" + version + " · " + p.optString("author", "社区");
        if (installed) sub += " · 已安装";
        titleCol.addView(Ui.text(this, sub, 12, installed ? Ui.OK : Ui.SUB, false), lp(-2, -2));
        head.addView(titleCol, lp(0, -2, 1f));

        TextView action = pillButton(installed ? "重新安装" : "安装",
                installed ? Ui.SURFACE : Ui.PRIMARY, installed ? Ui.INK : 0xFFFFFFFF, p);
        head.addView(action, lp(-2, -2));
        card.addView(head, lp(-1, -2));

        String summary = p.optString("summary", "");
        if (!TextUtils.isEmpty(summary)) {
            TextView s = Ui.text(this, summary, 13, Ui.SUB, false);
            s.setPadding(dp(1), dp(8), dp(1), 0);
            card.addView(s, lp(-1, -2));
        }
        JSONArray tags = p.optJSONArray("tags");
        if (tags != null && tags.length() > 0) {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < Math.min(tags.length(), 4); i++) {
                if (i > 0) sb.append(" · ");
                sb.append(tags.optString(i, ""));
            }
            TextView t = Ui.text(this, sb.toString(), 11, Ui.SUB, false);
            t.setPadding(dp(1), dp(6), dp(1), 0);
            card.addView(t, lp(-1, -2));
        }
        return card;
    }

    /** bg 传 null 表示主色浅底描边样式（等待中的按钮）。 */
    private TextView pillButton(String text, int bg, int fg, JSONObject pkg) {
        TextView t = Ui.text(this, text, 13, fg, true);
        t.setGravity(Gravity.CENTER);
        t.setBackground(bg == Ui.SURFACE
                ? Ui.card(this, bg, 10, Ui.LINE, 1)
                : Ui.fill(this, bg, 10));
        t.setPadding(dp(14), dp(8), dp(14), dp(8));
        Ui.press(t);
        if (pkg != null) t.setOnClickListener(v -> confirmInstall(pkg, t));
        return t;
    }

    private void confirmInstall(JSONObject pkg, TextView button) {
        String id = pkg.optString("id", "");
        String name = pkg.optString("name", id);
        boolean installed = pkg.optBoolean("installed", false);
        new AlertDialog.Builder(this)
                .setTitle(installed ? "重新安装？" : "安装？")
                .setMessage(installed
                        ? name + " 已在设备上。重新安装会换成市场最新版，你在插件里保存的设置会保留。"
                        : "安装 " + name + " 到当前桥接器？装完即时生效。")
                .setPositiveButton(installed ? "重装" : "安装", (d, w) -> install(id, name, button))
                .setNegativeButton("取消", null)
                .show();
    }

    private void install(String id, String name, TextView button) {
        button.setEnabled(false);
        button.setText("安装中…");
        new Thread(() -> {
            JSONObject payload = new JSONObject();
            try { payload.put("id", id); } catch (Exception ignore) { }
            Bridge.Resp r = http("POST", "/api/plugins/install", payload.toString().getBytes(StandardCharsets.UTF_8));
            runOnUiThread(() -> {
                button.setEnabled(true);
                if (r.ok()) {
                    Toast.makeText(this, "已安装 " + name, Toast.LENGTH_SHORT).show();
                    reload();
                } else {
                    button.setText("安装");
                    Toast.makeText(this, r.errorText(), Toast.LENGTH_LONG).show();
                }
            });
        }, "made.market.install").start();
    }

    // ================= HTTP（同 CloudSettingsActivity：云端 + 沙箱双重鉴权） =================

    private Bridge.Resp http(String method, String path, byte[] body) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(base + path).openConnection();
            conn.setConnectTimeout(10000);
            // 安装含下载归档（最大几百 KB），读超时放宽到 60s
            conn.setReadTimeout(60000);
            conn.setRequestMethod(method);
            conn.setRequestProperty("x-made-cloud-token", cloudToken == null ? "" : cloudToken);
            conn.setRequestProperty("Authorization", "Bearer " + token);
            if (!TextUtils.isEmpty(vibeToken)) conn.setRequestProperty("x-vibe-token", vibeToken);
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
