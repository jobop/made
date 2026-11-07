package com.made.app;

import android.content.Context;
import android.util.Log;

import org.json.JSONObject;

import java.security.SecureRandom;
import java.util.Arrays;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/**
 * 配对状态机，协议对齐固件 vibe_pairing.cpp：
 * 1. POST /pair/request {deviceId, deviceName, code(6位), nonce} -&gt; 202
 * 2. 电脑工作台确认（校验配对码）
 * 3. 轮询 GET /pair/status?deviceId=&amp;nonce= -&gt; approved/denied/expired/pending + token
 * 4. 重启恢复：GET /pair/verify?deviceId=&amp;nonce=，校验 HMAC-SHA256(token, ...)
 */
public final class Pairing {
    private static final String TAG = "made.pairing";
    private static final String PREFS = "made_pairing";
    private static final String K_DEVICE_ID = "device_id";
    private static final String K_URL = "url";
    private static final String K_TOKEN = "token";
    private static final String K_BRIDGE = "bridge_id";
    private static final String K_CLOUD_TOKEN = "cloud_token";
    private static final String K_CLOUD_SERVER = "cloud_server";
    private static final String MANUAL_SALT = "VIBE_BRIDGE_MANUAL_V2";

    public enum Phase { IDLE, REQUESTING, WAITING_APPROVAL, PAIRED, ERROR }

    public interface Listener {
        /** UI 线程回调。 */
        void onPairingUpdate(Phase phase, String message, String code);
    }

    private final Bridge bridge;
    private final Context context;
    private final SecureRandom random = new SecureRandom();

    private String deviceId = "";
    private String bridgeId = "";
    private volatile String pendingNonce = "";
    private volatile long pairStartMs = 0;
    private static final long PAIR_LIFETIME_MS = 120_000;

    public Pairing(Bridge bridge, Context context) {
        this.bridge = bridge;
        this.context = context;
        load();
    }

    public String deviceId() { return deviceId; }
    public String bridgeId() { return bridgeId; }

    private void load() {
        android.content.SharedPreferences p = context.getSharedPreferences(PREFS, 0);
        deviceId = p.getString(K_DEVICE_ID, "");
        if (deviceId.length() != 12) {
            StringBuilder sb = new StringBuilder(12);
            for (int i = 0; i < 6; i++) sb.append(String.format("%02x", random.nextInt(256)));
            deviceId = sb.toString();
            p.edit().putString(K_DEVICE_ID, deviceId).apply();
        }
        String url = p.getString(K_URL, "");
        String token = p.getString(K_TOKEN, "");
        bridgeId = p.getString(K_BRIDGE, "");
        bridge.setCloudToken(p.getString(K_CLOUD_TOKEN, ""));
        if (!url.isEmpty() && token.length() == 64) bridge.setEndpoint(url, token);
    }

    /** 云端码得：保存 cloud 凭据（登录后、配对前调用，重启后能恢复）。 */
    public void saveCloud(String cloudToken, String server) {
        context.getSharedPreferences(PREFS, 0).edit()
                .putString(K_CLOUD_TOKEN, cloudToken).putString(K_CLOUD_SERVER, server).apply();
        bridge.setCloudToken(cloudToken);
    }

    public String cloudServer() {
        return context.getSharedPreferences(PREFS, 0).getString(K_CLOUD_SERVER, "");
    }

    /** 已保存的 cloud 登录令牌（未登录为空串）。 */
    public String cloudToken() {
        return context.getSharedPreferences(PREFS, 0).getString(K_CLOUD_TOKEN, "");
    }

    public String savedUrl() {
        return context.getSharedPreferences(PREFS, 0).getString(K_URL, "");
    }

    public String savedPort() {
        String url = savedUrl();
        int s = url.lastIndexOf(':');
        // 带路径的 URL（云端 bridgeUrl）没有「主机:端口」形式，不回填手动框
        if (s > 0 && url.indexOf('/', s) > 0) return "";
        return s > 0 ? url.substring(s + 1) : "";
    }

    public String savedHost() {
        String url = savedUrl();
        int s = url.indexOf("://");
        int e = url.lastIndexOf(':');
        if (s < 0) return "";
        if (url.indexOf('/', s + 3) > 0) return "";
        return e > s + 3 ? url.substring(s + 3, e) : url.substring(s + 3);
    }

    public void forget() {
        context.getSharedPreferences(PREFS, 0).edit()
                .remove(K_URL).remove(K_TOKEN).remove(K_BRIDGE)
                .remove(K_CLOUD_TOKEN).remove(K_CLOUD_SERVER).apply();
        bridge.setEndpoint("", "");
        bridge.setCloudToken("");
        bridgeId = "";
    }

    private void persist(String url, String token, String bridgeIdIn) {
        context.getSharedPreferences(PREFS, 0).edit()
                .putString(K_URL, url).putString(K_TOKEN, token).putString(K_BRIDGE, bridgeIdIn).apply();
        bridge.setEndpoint(url, token);
        bridgeId = bridgeIdIn;
    }

    private static String hex(byte[] data) {
        StringBuilder sb = new StringBuilder(data.length * 2);
        for (byte b : data) sb.append(String.format("%02x", b));
        return sb.toString();
    }

    /** 用户取消配对等待：轮询线程在下一个 1.2s 周期自然退出。 */
    public void cancelPending() {
        pendingNonce = "";
    }

    /** 手动地址 entry（协议约束：HTTP 只允许局域网地址，公网必须 HTTPS —— 这里交由用户自觉，桥接器 /pair/verify 会兜底校验 authority）。 */
    public void startManualPair(String host, int port, boolean https, Listener listener) {
        startPairingUrl((https ? "https://" : "http://") + host + ":" + port, listener);
    }

    /** 完整 baseUrl（可含路径，如云端 bridgeUrl）发起配对。 */
    public void startPairingUrl(String baseUrl, Listener listener) {
        // 旧 bridgeId 只对上一轮配对有意义：云端换账号/沙箱重建（换 e2b 账号、
        // 沙箱冷启重建）后桥接器 bridgeId 必然变化，拿旧 id 否决新配对会让
        // 用户永远卡在「配对信息无效」。每轮配对的真门禁是配对码 + HMAC token，
        // 批准后 persist() 会写入新 id。
        bridgeId = "";
        String[] code = new String[1];
        byte[] nonceBytes = new byte[16];
        random.nextBytes(nonceBytes);
        pendingNonce = hex(nonceBytes);
        // 与固件一致：6 位等概率。nextInt(bound) 不会返回负值，
        // 也不像 Math.abs(nextInt()) 那样在 Integer.MIN_VALUE 上取模出负号。
        code[0] = String.format("%06d", random.nextInt(1000000));
        pairStartMs = System.currentTimeMillis();
        final String url = baseUrl.endsWith("/")
                ? baseUrl.substring(0, baseUrl.length() - 1) : baseUrl;
        listener.onPairingUpdate(Phase.REQUESTING, "正在连接电脑…", "");
        new Thread(() -> {
            try {
                JSONObject req = new JSONObject();
                req.put("deviceId", deviceId);
                req.put("deviceName", "码得（安卓）");
                req.put("code", code[0]);
                req.put("nonce", pendingNonce);
                Bridge.Resp r = bridge.exchangeNoAuth(url + "/pair/request", "POST",
                        req.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8),
                        "application/json");
                if (r.code == 202) {
                    listener.onPairingUpdate(Phase.WAITING_APPROVAL, "已找到电脑，请在电脑工作台核对配对码", code[0]);
                    pollApproval(baseUrl, listener);
                } else if (r.code == 403) {
                    listener.onPairingUpdate(Phase.ERROR, "在电脑工作台点击「添加设备」后重试", "");
                } else {
                    listener.onPairingUpdate(Phase.ERROR, "电脑配对请求失败：" + r.errorText(), "");
                }
            } catch (Exception e) {
                Log.w(TAG, "pair request", e);
                listener.onPairingUpdate(Phase.ERROR, "电脑连接失败", "");
            }
        }, "made-pair").start();
    }

    private void pollApproval(String baseUrl, Listener listener) {
        new Thread(() -> {
            while (System.currentTimeMillis() - pairStartMs < PAIR_LIFETIME_MS) {
                try {
                    Thread.sleep(1200);
                } catch (InterruptedException e) {
                    return;
                }
                if (pendingNonce.isEmpty()) return; // 已被取消/完成
                try {
                    Bridge.Resp r = bridge.exchangeNoAuth(
                            baseUrl + "/pair/status?deviceId=" + deviceId + "&nonce=" + pendingNonce,
                            "GET", null, null);
                    if (r.code == 200) {
                        JSONObject o = new JSONObject(r.body);
                        String status = o.optString("status");
                        if ("approved".equals(status)) {
                            String token = o.optString("token");
                            String bid = o.optString("bridgeId");
                            if (token.length() != 64 || (!bridgeId.isEmpty() && !bridgeId.equals(bid))) {
                                listener.onPairingUpdate(Phase.ERROR, "配对信息无效，请重试", "");
                                return;
                            }
                            persist(baseUrl, token, bid);
                            pendingNonce = "";
                            listener.onPairingUpdate(Phase.PAIRED, "已连接电脑", "");
                            return;
                        } else if ("denied".equals(status)) {
                            pendingNonce = "";
                            listener.onPairingUpdate(Phase.ERROR, "电脑拒绝了配对，请重试", "");
                            return;
                        } else if ("expired".equals(status)) {
                            pendingNonce = "";
                            listener.onPairingUpdate(Phase.ERROR, "配对已超时，请重试", "");
                            return;
                        }
                    }
                } catch (Exception e) {
                    Log.w(TAG, "poll status", e);
                }
            }
            if (!pendingNonce.isEmpty()) {
                pendingNonce = "";
                listener.onPairingUpdate(Phase.ERROR, "配对已超时，请重试", "");
            }
        }, "made-pair-poll").start();
    }

    /**
     * 恢复校验：用已存 token 验证桥接器身份。
     * 返回 true = 仍有效；false = 404（桥接器不再认识此设备，需要重新配对）；
     * 其他失败（超时/403 等）保持乐观，按固件语义视为暂时不可用。
     */
    public boolean verifySaved() {
        if (!bridge.isPaired()) return false;
        try {
            byte[] challengeBytes = new byte[16];
            random.nextBytes(challengeBytes);
            String challenge = hex(challengeBytes);
            Bridge.Resp r = bridge.exchangeNoAuth(
                    "/pair/verify?deviceId=" + deviceId + "&nonce=" + challenge,
                    "GET", null, null);
            if (r.code == 404) return false; // 桥接器已解除配对
            if (r.code != 200) return bridge.isPaired(); // 暂时不可用，保留凭据
            JSONObject o = new JSONObject(r.body);
            String bid = o.optString("bridgeId");
            String mac = o.optString("mac");
            String authority = authority(bridge.baseUrl());
            if (!bid.equals(bridgeId)) return false;
            String signed = MANUAL_SALT + "\n" + deviceId + "\n" + challenge + "\n" + bid + "\n" + authority;
            Mac hmac = Mac.getInstance("HmacSHA256");
            byte[] key = hexToBytes(bridge.token());
            hmac.init(new SecretKeySpec(key, "HmacSHA256"));
            byte[] expected = hmac.doFinal(signed.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            byte[] received = hexToBytes(mac);
            return expected.length == received.length && Arrays.equals(expected, received);
        } catch (Exception e) {
            Log.w(TAG, "verify", e);
            return true; // 网络异常按乐观处理，由后续请求自然暴露
        }
    }

    /** 与固件 authority_for_url 一致：小写 host:port，缺省端口去除。 */
    private static String authority(String url) {
        int s = url.indexOf("://");
        if (s < 0) return "";
        String a = url.substring(s + 3).toLowerCase(java.util.Locale.ROOT);
        if (a.endsWith(":80") && !url.startsWith("https")) a = a.substring(0, a.length() - 3);
        if (a.endsWith(":443") && url.startsWith("https")) a = a.substring(0, a.length() - 4);
        return a;
    }

    private static byte[] hexToBytes(String hex) {
        if (hex == null || hex.length() % 2 != 0) return new byte[0];
        byte[] out = new byte[hex.length() / 2];
        for (int i = 0; i < out.length; i++) {
            out[i] = (byte) Integer.parseInt(hex.substring(i * 2, i * 2 + 2), 16);
        }
        return out;
    }
}
