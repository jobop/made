package com.made.app;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.json.JSONObject;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;

/**
 * 内网桥接器发现：UDP 广播 VIBE_DISCOVER_V3（与桌面工作台同一协议，
 * 见 bridge/src/server.mjs 的 discovery socket）。桥接器在 8789/udp 监听，
 * 收到广播后回 JSON {type:"VIBE_BRIDGE_V3", port, bridgeId, name, pairingOpen}，
 * 同时会自动打开配对窗口。
 * AP 隔离 / 广播受限的网络收不到应答 —— 此时 UI 回落到手动输入地址。
 */
public final class LanScanner {
    private static final String TAG = "made.scan";
    private static final int DISCOVERY_PORT = 8789;
    private static final byte[] REQUEST =
            "VIBE_DISCOVER_V3".getBytes(StandardCharsets.US_ASCII);
    private static final long WINDOW_MS = 2800;

    /** 一台被发现 的桥接器。 */
    public static final class Found {
        public String ip = "", name = "", bridgeId = "";
        public int port = 8788;
    }

    public interface Listener {
        /** UI 线程：发现一台（同一 bridgeId 去重后）。 */
        void onFound(Found f);

        /** UI 线程：扫描窗口结束。 */
        void onDone(int count);
    }

    private final Handler ui = new Handler(Looper.getMainLooper());
    private Thread worker;
    private volatile boolean cancelled;

    /** 开始一轮扫描（重复调用会先取消上一轮）。 */
    public void scan(Context context, Listener listener) {
        cancel();
        cancelled = false;
        WifiManager.MulticastLock lock = null;
        try {
            WifiManager wm = (WifiManager) context.getApplicationContext()
                    .getSystemService(Context.WIFI_SERVICE);
            if (wm != null) {
                lock = wm.createMulticastLock("made-scan");
                lock.setReferenceCounted(false);
                lock.acquire();
            }
        } catch (Exception ignored) { }
        final WifiManager.MulticastLock mlock = lock;
        worker = new Thread(() -> run(listener, mlock), "made-scan");
        worker.setDaemon(true);
        worker.start();
    }

    public void cancel() {
        cancelled = true;
        Thread t = worker;
        worker = null;
        if (t != null) t.interrupt();
    }

    private void run(Listener listener, WifiManager.MulticastLock lock) {
        int found = 0;
        Set<String> seen = new LinkedHashSet<>();
        try (DatagramSocket socket = new DatagramSocket()) {
            socket.setBroadcast(true);
            socket.setSoTimeout(240);
            Set<InetAddress> targets = broadcastTargets();
            byte[] buf = new byte[2048];
            long deadline = System.currentTimeMillis() + WINDOW_MS;
            long lastSend = 0;
            while (!cancelled && System.currentTimeMillis() < deadline) {
                if (System.currentTimeMillis() - lastSend > 650) {
                    for (InetAddress target : targets) {
                        try {
                            socket.send(new DatagramPacket(REQUEST, REQUEST.length,
                                    target, DISCOVERY_PORT));
                        } catch (Exception ignored) { }
                    }
                    lastSend = System.currentTimeMillis();
                }
                DatagramPacket packet = new DatagramPacket(buf, buf.length);
                try {
                    socket.receive(packet);
                } catch (java.net.SocketTimeoutException e) {
                    continue;
                }
                Found f = parse(new String(packet.getData(), 0, packet.getLength(),
                        StandardCharsets.UTF_8), packet.getAddress());
                if (f == null) continue;
                String key = f.bridgeId.isEmpty() ? f.ip : f.bridgeId;
                if (!seen.add(key)) continue;
                found++;
                ui.post(() -> listener.onFound(f));
            }
        } catch (Exception e) {
            Log.w(TAG, "scan", e);
        } finally {
            if (lock != null) {
                try { lock.release(); } catch (Exception ignored) { }
            }
        }
        final int count = found;
        if (!cancelled) ui.post(() -> listener.onDone(count));
    }

    private static Found parse(String body, InetAddress from) {
        try {
            JSONObject o = new JSONObject(body);
            if (!"VIBE_BRIDGE_V3".equals(o.optString("type"))) return null;
            Found f = new Found();
            f.ip = from.getHostAddress();
            f.port = o.optInt("port", 8788);
            f.name = o.optString("name", "码得桥接器");
            f.bridgeId = o.optString("bridgeId");
            return f;
        } catch (Exception e) {
            return null;
        }
    }

    /** 255.255.255.255 + 每个网卡的定向广播地址（多网卡/热点场景都覆盖）。 */
    private static Set<InetAddress> broadcastTargets() {
        Set<InetAddress> out = new LinkedHashSet<>();
        try {
            out.add(InetAddress.getByName("255.255.255.255"));
            for (NetworkInterface nif : java.util.Collections
                    .list(NetworkInterface.getNetworkInterfaces())) {
                if (!nif.isUp() || nif.isLoopback()) continue;
                for (InterfaceAddress ia : nif.getInterfaceAddresses()) {
                    InetAddress b = ia.getBroadcast();
                    if (b != null) out.add(b);
                }
            }
        } catch (Exception ignored) { }
        return out;
    }
}
