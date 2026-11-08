package android.content;
public final class Context {
    private final java.io.File files;
    public Context() {
        try { files = java.nio.file.Files.createTempDirectory("made-prefs-test-").toFile(); }
        catch (java.io.IOException e) { throw new RuntimeException(e); }
    }
    public java.io.File getFilesDir() { return files; }
    private final java.util.Map<String, SharedPreferences> prefs = new java.util.HashMap<>();
    public SharedPreferences getSharedPreferences(String name, int mode) {
        return prefs.computeIfAbsent(name, key -> new SharedPreferences());
    }
}
