package android.content;
public final class Context {
    private final java.util.Map<String, SharedPreferences> prefs = new java.util.HashMap<>();
    public SharedPreferences getSharedPreferences(String name, int mode) {
        return prefs.computeIfAbsent(name, key -> new SharedPreferences());
    }
}
