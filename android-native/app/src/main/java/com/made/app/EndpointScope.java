package com.made.app;

import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Locale;

/** URL identity shared by credential routing, pairing and persistent H5 caches. */
final class EndpointScope {
    private EndpointScope() { }

    private static URI parse(String value) throws Exception {
        URI uri = new URI(value);
        String scheme = uri.getScheme();
        if (uri.getHost() == null || uri.getRawUserInfo() != null
                || !("http".equalsIgnoreCase(scheme) || "https".equalsIgnoreCase(scheme))) {
            throw new IllegalArgumentException("Invalid endpoint");
        }
        return uri;
    }

    static String authority(String value) {
        try {
            URI uri = parse(value);
            String host = uri.getHost().toLowerCase(Locale.ROOT);
            int port = uri.getPort();
            if (port == ("https".equalsIgnoreCase(uri.getScheme()) ? 443 : 80)) port = -1;
            return host + (port < 0 ? "" : ":" + port);
        } catch (Exception e) { return ""; }
    }

    static boolean belongsTo(String request, String server) {
        try {
            URI target = parse(request), root = parse(server);
            if (!target.getScheme().equalsIgnoreCase(root.getScheme())
                    || !authority(request).equals(authority(server))) return false;
            String prefix = root.normalize().getPath();
            while (prefix.endsWith("/")) prefix = prefix.substring(0, prefix.length() - 1);
            String path = target.normalize().getPath();
            // Encoded separators/dots must not bypass a deployment's URL prefix.
            if (target.getRawPath().matches("(?i).*(%2e|%2f|%5c|%00).*")) return false;
            return path.equals(prefix) || path.startsWith(prefix + "/");
        } catch (Exception e) { return false; }
    }

    static String cacheKey(String endpoint, String deviceToken) {
        try {
            URI uri = parse(endpoint);
            String path = uri.getRawPath();
            while (path.endsWith("/")) path = path.substring(0, path.length() - 1);
            String identity = uri.getScheme().toLowerCase(Locale.ROOT) + "://"
                    + authority(endpoint) + path + "\n" + deviceToken;
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(identity.getBytes(StandardCharsets.UTF_8));
            StringBuilder hex = new StringBuilder();
            for (byte b : digest) hex.append(String.format(Locale.ROOT, "%02x", b & 255));
            return hex.toString();
        } catch (Exception e) { throw new IllegalArgumentException("Invalid cache endpoint", e); }
    }
}
