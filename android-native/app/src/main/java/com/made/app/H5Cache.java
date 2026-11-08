package com.made.app;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/** Static games belong to an endpoint/account, not to its renewable pairing credential. */
final class H5Cache {
    private H5Cache() { }
    private static final long MAX_FILE = 8L * 1024 * 1024;

    static String key(String endpoint) { return EndpointScope.cacheKey(endpoint, ""); }

    static File root(File files, String endpoint) {
        return new File(files, "h5cache/.v2/" + key(endpoint));
    }

    /** Called on startup with the saved pairing, before an account can be switched. */
    static synchronized File restore(File files, String endpoint, String token, boolean claimLegacy) throws IOException {
        File oldRoot = new File(files, "h5cache");
        File target = root(files, endpoint);
        File scoped = new File(oldRoot, EndpointScope.cacheKey(endpoint, token));
        copyDirectory(scoped, target);
        File owner = new File(oldRoot, ".legacy-owner");
        if (claimLegacy && !owner.exists()) write(owner, key(endpoint).getBytes(StandardCharsets.UTF_8));
        if (owner.isFile() && key(endpoint).equals(new String(Files.readAllBytes(owner.toPath()), StandardCharsets.UTF_8))) {
            File[] entries = oldRoot.listFiles();
            if (entries != null) for (File entry : entries) {
                String name = entry.getName();
                if (name.equals("installed.json") || name.matches("[a-z0-9][a-z0-9_-]{0,39}(\\.ver)?")) {
                    if (entry.isDirectory()) copyDirectory(entry, new File(target, name));
                    else copyFile(entry, new File(target, name));
                }
            }
        }
        return target;
    }

    private static void copyDirectory(File source, File target) throws IOException {
        if (!source.isDirectory() || Files.isSymbolicLink(source.toPath())) return;
        File[] children = source.listFiles();
        if (children == null) return;
        for (File child : children) {
            if (Files.isSymbolicLink(child.toPath())) continue;
            File dest = new File(target, child.getName());
            if (child.isDirectory()) copyDirectory(child, dest);
            else copyFile(child, dest);
        }
    }

    private static void copyFile(File source, File target) throws IOException {
        if (!source.isFile() || Files.isSymbolicLink(source.toPath()) || source.length() > MAX_FILE
                || (target.isFile() && target.length() > 0)) return;
        write(target, Files.readAllBytes(source.toPath()));
    }

    static boolean hasPage(File directory) {
        File page = new File(directory, "index.html");
        return page.isFile() && page.length() > 0;
    }

    /** A failed download/write must never truncate the last playable copy. */
    static void write(File target, byte[] bytes) throws IOException {
        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create cache directory");
        File temp = File.createTempFile(".h5-", ".tmp", parent);
        try {
            Files.write(temp.toPath(), bytes);
            Files.move(temp.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
        } finally { temp.delete(); }
    }
}
