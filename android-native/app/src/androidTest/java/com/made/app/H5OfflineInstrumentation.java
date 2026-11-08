package com.made.app;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Intent;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.WebView;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/** Real WebView regression: download online, pause backend, reopen and play without waking it. */
public final class H5OfflineInstrumentation extends Instrumentation {
    private final AtomicBoolean paused = new AtomicBoolean(), brokenUpdate = new AtomicBoolean();
    private final AtomicInteger requests = new AtomicInteger(), pausedStaticRequests = new AtomicInteger();
    private volatile boolean serving = true;
    private ServerSocket server;
    private String base;

    private static void check(boolean condition, String message) { if (!condition) throw new AssertionError(message); }
    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); start(); }
    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            server = new ServerSocket(0);
            base = "http://127.0.0.1:" + server.getLocalPort();
            Thread thread = new Thread(this::serve, "h5-offline-test-server");
            thread.setDaemon(true); thread.start();
            for (int index = 0; index < 2; index++) scenario(index);
            result.putString("stream", "PASS: two real WebView games survive pause, re-pairing and failed refresh; explicit offline opens make zero network requests\n");
            finish(Activity.RESULT_OK, result);
        } catch (Throwable error) {
            result.putString("stream", "FAIL: " + android.util.Log.getStackTraceString(error));
            finish(Activity.RESULT_CANCELED, result);
        } finally {
            serving = false;
            try { if (server != null) server.close(); } catch (Exception ignored) { }
        }
    }

    private void scenario(int index) throws Exception {
        String endpoint = base + "/bridge/account-" + index;
        String id = "offline-game-" + index;
        paused.set(false); brokenUpdate.set(false);
        Activity first = open(endpoint, id, "old-token", false);
        ready(first);
        runOnMainSync(first::finish);
        waitForIdleSync();
        paused.set(true);
        int before = requests.get();
        Activity local = open(endpoint, id, "rotated-token", true);
        ready(local);
        check("1".equals(js(local, "document.querySelector('#play').click(); document.querySelector('#score').textContent")), "cached game remains interactive");
        check(requests.get() == before, "explicit pause must not make any request: before=" + before + ", after=" + requests.get());
        runOnMainSync(local::finish);
        Activity fallback = open(endpoint, id, "rotated-token", false);
        ready(fallback);
        check(pausedStaticRequests.get() == 0, "409 token response must fall back to cached static page/assets");
        runOnMainSync(fallback::finish);
        paused.set(false); brokenUpdate.set(true);
        Activity failedRefresh = open(endpoint, id, "rotated-token", false);
        ready(failedRefresh);
        runOnMainSync(failedRefresh::finish);
        paused.set(true);
        Activity preserved = open(endpoint, id, "rotated-token", true);
        ready(preserved);
        runOnMainSync(preserved::finish);
        Bundle progress = new Bundle(); progress.putString("stream", "PASS WebView " + index + ": " + endpoint + " / " + id + "\n"); sendStatus(0, progress);
    }

    private Activity open(String endpoint, String id, String token, boolean offline) {
        Intent intent = new Intent(getTargetContext(), H5Activity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.putExtra("base", endpoint).putExtra("dir", id).putExtra("name", id)
                .putExtra("token", token).putExtra("offline", offline);
        return startActivitySync(intent);
    }
    private WebView web(View view) {
        if (view instanceof WebView) return (WebView)view;
        if (view instanceof ViewGroup) for (int i = 0; i < ((ViewGroup)view).getChildCount(); i++) {
            WebView found = web(((ViewGroup)view).getChildAt(i)); if (found != null) return found;
        }
        return null;
    }
    private String js(Activity activity, String script) throws Exception {
        AtomicReference<String> answer = new AtomicReference<>(); CountDownLatch done = new CountDownLatch(1);
        runOnMainSync(() -> web(activity.getWindow().getDecorView()).evaluateJavascript(script, value -> { answer.set(value); done.countDown(); }));
        check(done.await(5, TimeUnit.SECONDS), "WebView evaluation timed out");
        String value = answer.get();
        return value != null && value.startsWith("\"") ? value.substring(1, value.length() - 1) : value;
    }
    private void ready(Activity activity) throws Exception {
        long deadline = System.currentTimeMillis() + 20000;
        while (!"true".equals(js(activity, "window.gameReady === true"))) {
            if (System.currentTimeMillis() > deadline) throw new AssertionError("game did not load: " + js(activity, "document.body.innerText"));
            Thread.sleep(100);
        }
    }

    private void serve() {
        while (serving) try (Socket client = server.accept()) {
            BufferedReader reader = new BufferedReader(new InputStreamReader(client.getInputStream(), StandardCharsets.UTF_8));
            String first = reader.readLine(); if (first == null) continue;
            String line; while ((line = reader.readLine()) != null && !line.isEmpty()) { }
            String path = first.split(" ")[1].split("\\?")[0];
            requests.incrementAndGet();
            android.util.Log.i("made.offline.test", "HTTP " + path + " paused=" + paused.get());
            int status = 200; String type = "application/json", body;
            if (path.endsWith("/token")) {
                if (paused.get()) { status = 409; body = "{\"error\":\"paused\"}"; }
                else body = "{\"token\":\"fixture\"}";
            } else if (path.endsWith("/api/installed")) {
                status = brokenUpdate.get() ? 503 : 200;
                body = "{\"h5apps\":[{\"id\":\"offline-game-0\",\"version\":\"1\"},{\"id\":\"offline-game-1\",\"version\":\"1\"}]}";
            } else {
                if (paused.get()) { pausedStaticRequests.incrementAndGet(); status = 409; }
                if (brokenUpdate.get()) status = 503;
                if (path.endsWith(".js")) { type = "text/javascript"; body = "window.gameReady=true;"; }
                else {
                    type = "text/html";
                    body = "<!doctype html><meta charset='utf-8'><button id='play' onclick=\"document.querySelector('#score').textContent=String(Number(document.querySelector('#score').textContent)+1)\">Play</button><div id='score'>0</div>"
                        + (path.contains("offline-game-1") ? "<script src='game.js'></script>" : "<script>window.gameReady=true;</script>");
                }
            }
            byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
            client.getOutputStream().write(("HTTP/1.1 " + status + (status == 200 ? " OK" : " Unavailable") + "\r\nContent-Type: " + type + "; charset=utf-8\r\nContent-Length: " + bytes.length + "\r\nConnection: close\r\n\r\n").getBytes(StandardCharsets.US_ASCII));
            client.getOutputStream().write(bytes); client.getOutputStream().flush();
        } catch (Exception error) { if (serving) throw new RuntimeException(error); }
    }
}
