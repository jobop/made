package com.made.app;

import android.content.Context;
import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

public final class BoundaryTest {
    static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder();
        for (byte b : bytes) result.append(String.format("%02x", b & 255));
        return result.toString();
    }
    public static void main(String[] args) throws Exception {
        check(EndpointScope.belongsTo("https://CLOUD.example:443/bridge/a", "https://cloud.example"), "default port equivalence");
        check(!EndpointScope.belongsTo("http://cloud.example/bridge/a", "https://cloud.example"), "no scheme downgrade");
        check(!EndpointScope.belongsTo("https://cloud.example.evil/bridge/a", "https://cloud.example"), "exact host");
        check(!EndpointScope.belongsTo("https://cloud.example/tenant-two/x", "https://cloud.example/tenant"), "prefix boundary");
        check(!EndpointScope.belongsTo("https://cloud.example/tenant/../other", "https://cloud.example/tenant"), "dot segment escape");
        check(!EndpointScope.belongsTo("https://cloud.example/tenant/%2e%2e/other", "https://cloud.example/tenant"), "encoded escape");
        String first = EndpointScope.cacheKey("https://cloud.example/bridge/a", "token-a");
        check(first.equals(EndpointScope.cacheKey("https://CLOUD.example:443/bridge/a/", "token-a")), "stable cache identity");
        check(!first.equals(EndpointScope.cacheKey("https://cloud.example/bridge/b", "token-a")), "account separation");
        check(!first.equals(EndpointScope.cacheKey("https://cloud.example/bridge/a", "token-b")), "pairing separation");
        check(!first.equals(EndpointScope.cacheKey("https://other.example/bridge/a", "token-a")), "endpoint separation");
        check(EndpointScope.authority("https://EXAMPLE.com:443/bridge/user").equals("example.com"), "authority excludes proxy path");
        check(EndpointScope.authority("http://[::1]:8788/x").equals("[::1]:8788"), "IPv6 authority");

        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        server.setExecutor(java.util.concurrent.Executors.newCachedThreadPool(r -> { Thread t = new Thread(r); t.setDaemon(true); return t; }));
        String base = "http://127.0.0.1:" + server.getAddress().getPort();
        AtomicReference<String> sent = new AtomicReference<>();
        AtomicInteger hits = new AtomicInteger(), mode = new AtomicInteger();
        CountDownLatch entered = new CountDownLatch(1), release = new CountDownLatch(1);
        String deviceId = "0123456789ab", token = "11".repeat(32), bid = "bridge-one";
        server.createContext("/", exchange -> {
            try {
                String path = exchange.getRequestURI().getPath();
                sent.set(exchange.getRequestHeaders().getFirst("x-made-cloud-token"));
                int code = 200; String body = "{}";
                if (path.endsWith("/pair/verify")) {
                    hits.incrementAndGet();
                    if (mode.get() == 1) code = 404;
                    else if (mode.get() == 2) code = 503;
                    else {
                        String nonce = null;
                        for (String field : exchange.getRequestURI().getQuery().split("&")) if (field.startsWith("nonce=")) nonce = field.substring(6);
                        String authority = path.startsWith("/bridge/") ? "8787-sandbox.example" : EndpointScope.authority(base);
                        Mac hmac = Mac.getInstance("HmacSHA256");
                        hmac.init(new SecretKeySpec(java.util.HexFormat.of().parseHex(token), "HmacSHA256"));
                        String mac = hex(hmac.doFinal(("VIBE_BRIDGE_MANUAL_V2\n" + deviceId + "\n" + nonce + "\n" + bid + "\n" + authority).getBytes(StandardCharsets.UTF_8)));
                        if (mode.get() == 3) mac = "00".repeat(32);
                        if (mode.get() == 4) mac = "not-hex";
                        body = new org.json.JSONObject().put("bridgeId", bid).put("authority", authority).put("mac", mac).toString();
                    }
                } else {
                    exchange.getResponseHeaders().add("x-made-cloud-token", "renewed");
                    if (path.equals("/redirect")) { code = 302; exchange.getResponseHeaders().add("Location", base + "/landing"); }
                    if (path.equals("/fail")) code = 401;
                    if (path.equals("/slow")) { entered.countDown(); release.await(); }
                }
                byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
                exchange.sendResponseHeaders(code, bytes.length);
                exchange.getResponseBody().write(bytes);
            } catch (Exception error) { throw new RuntimeException(error); }
            finally { exchange.close(); }
        });
        server.start();
        try {
            Bridge bridge = new Bridge();
            AtomicInteger renewed = new AtomicInteger();
            bridge.setCloudTokenListener(value -> renewed.incrementAndGet());
            bridge.setCloudCredentials(base + "/trusted", "original");
            bridge.exchangeNoAuth(base + "/foreign", "GET", null, null);
            check(sent.get() == null && bridge.cloudToken().equals("original") && renewed.get() == 0, "foreign endpoint cannot receive or replace JWT");
            bridge.exchangeNoAuth(base + "/trusted/request", "GET", null, null);
            check("original".equals(sent.get()) && bridge.cloudToken().equals("renewed") && renewed.get() == 1, "trusted renewal");
            bridge.setCloudCredentials(base, "original");
            bridge.exchangeNoAuth(base + "/fail", "GET", null, null);
            check(bridge.cloudToken().equals("original"), "error response cannot renew");
            check(bridge.exchangeNoAuth(base + "/redirect", "GET", null, null).code == 302, "credential requests do not follow redirects");
            Thread worker = new Thread(() -> bridge.exchangeNoAuth(base + "/slow", "GET", null, null));
            worker.start(); check(entered.await(5, java.util.concurrent.TimeUnit.SECONDS), "slow request started");
            bridge.setCloudCredentials("", ""); release.countDown(); worker.join(5000);
            check(bridge.cloudToken().isEmpty(), "stale response cannot undo logout");
            check(!bridge.renewCloudToken(base, "original", "late"), "stale proactive refresh rejected");
            for (String endpoint : new String[] {base, base + "/bridge/account"}) {
                Context context = new Context();
                context.getSharedPreferences("made_pairing", 0).edit().putString("device_id", deviceId)
                    .putString("url", endpoint).putString("token", token).putString("bridge_id", bid)
                    .putString("cloud_server", base).putString("cloud_token", "cloud-jwt").apply();
                Pairing pairing = new Pairing(new Bridge(), context);
                mode.set(0); check(pairing.verifySaved(), "valid HMAC at " + endpoint);
                mode.set(1); check(!pairing.verifySaved(), "revocation detected");
                mode.set(2); check(pairing.verifySaved(), "temporary server failure preserves saved pairing");
                mode.set(3); check(!pairing.verifySaved(), "wrong HMAC rejected");
                mode.set(4); check(!pairing.verifySaved(), "malformed HMAC rejected");
            }
            check(hits.get() == 10, "every saved-pair check reached server");
            System.out.println("PASS: cloud credential routing/renewal/logout, LAN/cloud pairing HMAC, cache/account isolation");
        } finally { release.countDown(); server.stop(0); }
    }
}
