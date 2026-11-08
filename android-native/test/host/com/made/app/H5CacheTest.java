package com.made.app;

import java.io.File;
import java.nio.file.Files;
import java.nio.charset.StandardCharsets;

public final class H5CacheTest {
    static void check(boolean ok, String message) { if (!ok) throw new AssertionError(message); }
    static String read(File f) throws Exception { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); }
    static void put(File root, String path, String value) throws Exception {
        H5Cache.write(new File(root, path), value.getBytes(StandardCharsets.UTF_8));
    }
    public static void main(String[] args) throws Exception {
        File files = Files.createTempDirectory("made-h5-migration-").toFile();
        try {
            File raw = new File(files, "h5cache");
            put(raw, "falling-blocks/index.html", "old-single-file-game");
            put(raw, "falling-blocks.ver", "1.0");
            put(raw, "installed.json", "old-installed-list");
            String a = "https://cloud.example/bridge/account-a", b = "https://cloud.example/bridge/account-b";
            File migrated = H5Cache.restore(files, a, "old-pair-token", true);
            check(H5Cache.hasPage(new File(migrated, "falling-blocks")), "upgrade retains legacy playable page without network");
            check(read(new File(migrated, "installed.json")).equals("old-installed-list"), "legacy app list restored");
            check(read(new File(migrated, "falling-blocks.ver")).equals("1.0"), "legacy version restored");
            File rotated = H5Cache.restore(files, a, "new-pair-token", false);
            check(rotated.equals(migrated) && H5Cache.hasPage(new File(rotated, "falling-blocks")), "re-pairing does not hide downloaded games");
            File foreign = H5Cache.restore(files, b, "another-token", true);
            check(!H5Cache.hasPage(new File(foreign, "falling-blocks")), "legacy cache belongs only to saved account at upgrade");
            File scoped = new File(raw, EndpointScope.cacheKey(b, "token-b"));
            put(scoped, "tile-game/index.html", "scoped-game");
            put(scoped, "tile-game/assets/game.js", "nested-script");
            put(scoped, "tile-game.ver", "2.0");
            put(scoped, "installed.json", "scoped-installed-list");
            File v2 = H5Cache.restore(files, b, "token-b", false);
            check(H5Cache.hasPage(new File(v2, "tile-game")), "upgrade from token-scoped cache works offline");
            check(read(new File(v2, "tile-game/assets/game.js")).equals("nested-script"), "all static assets restored");
            check(read(new File(v2, "installed.json")).equals("scoped-installed-list"), "scoped list restored");
            put(v2, "tile-game/index.html", "updated-game");
            H5Cache.restore(files, b, "token-b", false);
            check(read(new File(v2, "tile-game/index.html")).equals("updated-game"), "migration never overwrites newer cache");
            check(!H5Cache.hasPage(new File(migrated, "tile-game")), "second account resources stay isolated");
            put(v2, "empty/index.html", "");
            check(!H5Cache.hasPage(new File(v2, "empty")), "empty failed download is not playable");
            System.out.println("PASS: two-account H5 legacy/token-scoped migration, token rotation, nested assets, cache retention");
        } finally {
            try (java.util.stream.Stream<java.nio.file.Path> paths = Files.walk(files.toPath())) {
                paths.sorted(java.util.Comparator.reverseOrder()).forEach(path -> { try { Files.delete(path); } catch (Exception e) { throw new RuntimeException(e); } });
            }
        }
    }
}
