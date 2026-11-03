package com.made.app;

import android.app.Activity;
import android.graphics.Typeface;
import android.os.Bundle;
import android.text.InputType;
import android.text.TextUtils;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
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
import java.util.List;

/**
 * 云端桥接器设置（仅云端沙箱模式入口可见）。
 * 读取桥接器 /api/installed 的 settingsSpec（插件包声明的配置字段）动态渲染表单，
 * 保存走 /api/plugins/settings。配置项完全由插件 spec 驱动，App 不硬编码任何插件字段。
 */
public final class CloudSettingsActivity extends Activity {

    private String base, token, cloudToken;
    private volatile String vibeToken = ""; // POST /api/* 需要的本机操作令牌
    private LinearLayout list;
    private TextView status;
    private final List<PluginCard> cards = new ArrayList<>();

    private static final class PluginCard {
        JSONObject plugin;
        boolean h5 = false; // h5 应用：无启用开关，只有设置表单
        LinearLayout body;   // 可展开的表单区
        boolean expanded = false;
        final List<EditText> inputs = new ArrayList<>();
        final List<JSONObject> specs = new ArrayList<>();
        final List<String> cloudKeys = new ArrayList<>();   // 云端提供的字段 key
        final List<String> originals = new ArrayList<>();   // 每个输入框的初始值（判定用户是否改过）
    }

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

        // 顶栏：返回 + 标题
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setBackgroundColor(Ui.SURFACE);
        bar.setPadding(dp(10), dp(10), dp(18), dp(10));
        TextView back = Ui.text(this, "‹", 26, Ui.INK, false);
        back.setPadding(dp(10), 0, dp(14), 0);
        back.setOnClickListener(v -> finish());
        bar.addView(back, lp(-2, -2));
        bar.addView(Ui.text(this, getIntent().getStringExtra("title") == null
                ? "云端桥接器设置" : getIntent().getStringExtra("title"), 17, Ui.INK, true), lp(0, -2, 1f));
        TextView market = Ui.text(this, "＋ 添加插件", 14, Ui.PRIMARY, true);
        market.setPadding(dp(10), dp(6), dp(4), dp(6));
        market.setOnClickListener(v -> {
            android.content.Intent it = new android.content.Intent(this, MarketActivity.class);
            it.putExtra("base", base);
            it.putExtra("token", token);
            it.putExtra("cloudToken", cloudToken);
            startActivity(it);
        });
        Ui.press(market);
        bar.addView(market, lp(-2, -2));
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
            // 先取本机操作令牌（POST /api/* 必须），再拉插件列表
            Bridge.Resp s = http("GET", "/api/session", null);
            if (s.ok()) {
                try { vibeToken = new JSONObject(s.body).optString("token"); } catch (Exception ignore) { }
            }
            Bridge.Resp r = http("GET", "/api/installed", null);
            List<PluginCard> parsed = new ArrayList<>();
            int h5Count = 0;
            String error = r.ok() ? "" : r.errorText();
            if (r.ok()) {
                try {
                    JSONObject data = new JSONObject(r.body);
                    JSONArray arr = data.optJSONArray("plugins");
                    if (arr != null) {
                        for (int i = 0; i < arr.length(); i++) {
                            JSONObject p = arr.optJSONObject(i);
                            if (p == null) continue;
                            PluginCard c = new PluginCard();
                            c.plugin = p;
                            JSONArray spec = p.optJSONArray("settingsSpec");
                            if (spec != null) {
                                for (int k = 0; k < spec.length(); k++) {
                                    JSONObject f = spec.optJSONObject(k);
                                    if (f != null) c.specs.add(f);
                                }
                            }
                            parsed.add(c);
                        }
                    }
                    JSONArray h5 = data.optJSONArray("h5apps");
                    for (int i = 0; h5 != null && i < h5.length(); i++) {
                        JSONObject p = h5.optJSONObject(i);
                        if (p == null) continue;
                        PluginCard c = new PluginCard();
                        c.plugin = p;
                        c.h5 = true;
                        h5Count++;
                        JSONArray spec = p.optJSONArray("settingsSpec");
                        if (spec != null) {
                            for (int k = 0; k < spec.length(); k++) {
                                JSONObject f = spec.optJSONObject(k);
                                if (f != null) c.specs.add(f);
                            }
                        }
                        parsed.add(c);
                    }
                } catch (Exception e) {
                    error = "数据解析失败";
                }
            }
            final String err = error;
            final List<PluginCard> result = parsed;
            final int h5Final = h5Count;
            runOnUiThread(() -> render(err, result, h5Final));
        }, "made.cloudsettings").start();
    }

    private void render(String error, List<PluginCard> parsed, int h5Count) {
        cards.clear();
        cards.addAll(parsed);
        list.removeAllViews();
        if (!TextUtils.isEmpty(error)) {
            status.setText("加载失败：" + error);
            TextView retry = pillButton("重试", Ui.SURFACE, Ui.INK);
            retry.setOnClickListener(v -> reload());
            list.addView(retry, lp(-1, -2));
            return;
        }
        status.setText(cards.isEmpty() ? "桥接器上还没有可设置的插件"
                : "设置保存在桥接器上，改动即时生效" + (h5Count > 0 ? " · 含 " + h5Count + " 个 H5 应用" : ""));
        boolean anyCloud = false;
        for (PluginCard c : cards) {
            list.addView(pluginCard(c), lp(-1, -2));
            if (!c.cloudKeys.isEmpty()) anyCloud = true;
        }
        if (anyCloud) {
            TextView hint = Ui.text(this, "☁ 模型相关配置已由云端自动提供，什么都不填即可使用；想换成自己的模型服务，直接改写对应字段。", 12, Ui.PRIMARY, false);
            hint.setPadding(dp(4), dp(10), dp(4), dp(2));
            list.addView(hint, 0, lp(-1, -2));
        }
    }

    private View pluginCard(PluginCard c) {
        JSONObject p = c.plugin;
        String id = p.optString("id", "");
        String label = c.h5 ? p.optString("name", id) : p.optString("label", "");
        if (TextUtils.isEmpty(label)) label = id.isEmpty() ? p.optString("file", "未知插件") : id;
        boolean available = c.h5 || p.optBoolean("available", false);
        boolean enabled = p.optBoolean("enabled", false);
        boolean modelKind = "model".equalsIgnoreCase(p.optString("kind", ""));

        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(Ui.card(this, Ui.SURFACE, 12, Ui.LINE, 1));
        card.setPadding(dp(14), dp(12), dp(14), dp(12));

        // 头行：名称 + 状态 + 开关
        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout titleCol = new LinearLayout(this);
        titleCol.setOrientation(LinearLayout.VERTICAL);
        titleCol.addView(Ui.text(this, label, 15, Ui.INK, true), lp(-2, -2));
        String sub = c.h5 ? ("H5 应用" + (p.optString("version", "").isEmpty() ? ""
                : " · v" + p.optString("version", "")))
                : modelKind ? "模型助手" : p.optString("kind", "插件");
        if (!c.h5 && !available) sub += " · 不可用";
        titleCol.addView(Ui.text(this, sub, 12, !c.h5 && !available ? Ui.ERR : Ui.SUB, false), lp(-2, -2));
        head.addView(titleCol, new LinearLayout.LayoutParams(0, -2, 1f));

        if (!c.h5) {
            Switch sw = new Switch(this);
            sw.setEnabled(available && !id.isEmpty());
            sw.setChecked(enabled);
            sw.setOnCheckedChangeListener((b, checked) -> {
                if (b.isPressed()) toggleEnabled(id, checked, sw);
            });
            head.addView(sw, lp(-2, -2));
        }
        card.addView(head, lp(-1, -2));

        // 可展开的配置表单
        c.body = new LinearLayout(this);
        c.body.setOrientation(LinearLayout.VERTICAL);
        c.body.setVisibility(c.expanded ? View.VISIBLE : View.GONE);
        c.cloudKeys.clear();
        c.originals.clear();
        JSONObject sources = p.optJSONObject("settingsSources");
        if (!c.specs.isEmpty()) {
            c.body.addView(Ui.text(this, "配置项", 12, Ui.SUB, false), lp(-2, -2));
            JSONObject saved = p.optJSONObject("settings");
            for (JSONObject spec : c.specs) {
                String key = spec.optString("key", "");
                String fieldLabel = spec.optString("label", key);
                boolean fromCloud = sources != null && "cloud".equals(sources.optString(key, ""));
                if (fromCloud) c.cloudKeys.add(key);
                EditText et = new EditText(this);
                et.setInputType(InputType.TYPE_CLASS_TEXT);
                et.setTextSize(14);
                et.setHint(fieldLabel + (spec.optBoolean("required") ? "（必填）" : ""));
                et.setBackground(Ui.fill(this, Ui.SOFT, 8));
                et.setPadding(dp(10), dp(9), dp(10), dp(9));
                String current = saved == null ? "" : saved.optString(key, "");
                if (TextUtils.isEmpty(current)) current = spec.optString("default", "");
                et.setText(current);
                c.inputs.add(et);
                c.originals.add(current);
                LinearLayout.LayoutParams etLp = lp(-1, -2);
                etLp.topMargin = dp(8);
                c.body.addView(et, etLp);
                String help = spec.optString("help", "");
                if (fromCloud) {
                    TextView tag = Ui.text(this, "☁ 云端提供 · 可改写为你自己的", 11, Ui.PRIMARY, true);
                    tag.setPadding(dp(2), dp(3), 0, 0);
                    c.body.addView(tag, lp(-1, -2));
                }
                if (!TextUtils.isEmpty(help)) {
                    TextView h = Ui.text(this, help, 11, Ui.SUB, false);
                    h.setPadding(dp(2), dp(3), 0, 0);
                    c.body.addView(h, lp(-1, -2));
                }
            }
            TextView save = pillButton("保存设置", Ui.PRIMARY, 0xFFFFFFFF);
            save.setOnClickListener(v -> saveSettings(c));
            LinearLayout.LayoutParams saveLp = lp(-1, -2);
            saveLp.topMargin = dp(12);
            c.body.addView(save, saveLp);
        }
        card.addView(c.body, lp(-1, -2));

        // 点头部展开/收起表单
        View.OnClickListener toggle = v -> {
            c.expanded = !c.expanded && !c.specs.isEmpty();
            c.body.setVisibility(c.expanded ? View.VISIBLE : View.GONE);
        };
        head.setOnClickListener(toggle);
        if (c.specs.isEmpty()) card.setOnClickListener(toggle); // 无配置项时整卡无动作
        return card;
    }

    private TextView pillButton(String text, int bg, int fg) {
        TextView t = Ui.text(this, text, 14, fg, true);
        t.setGravity(Gravity.CENTER);
        t.setBackground(bg == Ui.SURFACE
                ? Ui.card(this, bg, 10, Ui.LINE, 1)
                : Ui.fill(this, bg, 10));
        t.setPadding(dp(12), dp(10), dp(12), dp(10));
        Ui.press(t);
        return t;
    }

    private void toggleEnabled(String id, boolean enabled, Switch sw) {
        new Thread(() -> {
            JSONObject payload = new JSONObject();
            try { payload.put("id", id).put("enabled", enabled); } catch (Exception ignore) { }
            Bridge.Resp r = http("POST", "/api/plugins/enabled", payload.toString().getBytes(StandardCharsets.UTF_8));
            runOnUiThread(() -> {
                if (!r.ok()) {
                    sw.setChecked(!enabled);
                    Toast.makeText(this, r.errorText(), Toast.LENGTH_SHORT).show();
                } else {
                    Toast.makeText(this, enabled ? "已启用" : "已停用", Toast.LENGTH_SHORT).show();
                }
            });
        }, "made.plugin.enabled").start();
    }

    private void saveSettings(PluginCard c) {
        String id = c.plugin.optString("id", "");
        if (id.isEmpty()) {
            Toast.makeText(this, "该插件无法配置", Toast.LENGTH_SHORT).show();
            return;
        }
        JSONObject settings = new JSONObject();
        for (int i = 0; i < c.specs.size(); i++) {
            String key = c.specs.get(i).optString("key", "");
            String value = c.inputs.get(i).getText().toString().trim();
            String pattern = c.specs.get(i).optString("pattern", "");
            if (!TextUtils.isEmpty(pattern) && !TextUtils.isEmpty(value) && !matchesQuietly(value, pattern)) {
                Toast.makeText(this, "「" + c.specs.get(i).optString("label", key) + "」格式不对", Toast.LENGTH_SHORT).show();
                return;
            }
            // 未改动的云端提供字段回传空串：留在「云端兑底」状态，不把沙箱令牌写死进配置
            boolean untouchedCloud = c.cloudKeys.contains(key)
                    && value.equals(i < c.originals.size() ? c.originals.get(i) : "");
            try { settings.put(key, untouchedCloud ? "" : value); } catch (Exception ignore) { }
        }
        JSONObject payload = new JSONObject();
        try { payload.put("id", id).put("settings", settings); } catch (Exception ignore) { }
        new Thread(() -> {
            Bridge.Resp r = http("POST", "/api/plugins/settings", payload.toString().getBytes(StandardCharsets.UTF_8));
            runOnUiThread(() -> Toast.makeText(this, r.ok() ? "已保存" : r.errorText(),
                    Toast.LENGTH_SHORT).show());
        }, "made.plugin.settings").start();
    }

    /**
     * 插件 settingsSpec 里的 pattern 由插件包提供、桥接器不校验，写成非法正则时
     * String.matches 会抛 PatternSyntaxException 直接把 UI 线程炸掉。这里退化为
     * 「放行 + 打日志」：错在插件配置，不该让用户的保存动作崩溃。
     */
    private static boolean matchesQuietly(String value, String pattern) {
        try {
            return value.matches(pattern);
        } catch (RuntimeException e) {
            Log.w("made.settings", "插件 pattern 非法，跳过校验：" + pattern, e);
            return true;
        }
    }

    // ================= HTTP（带云端 + 沙箱双重鉴权，经 cloud 代理直达沙箱桥接器） =================

    private Bridge.Resp http(String method, String path, byte[] body) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(base + path).openConnection();
            conn.setConnectTimeout(10000);
            conn.setReadTimeout(20000);
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
