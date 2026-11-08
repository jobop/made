package android.content;
public final class SharedPreferences {
    private final java.util.Map<String, String> data = new java.util.HashMap<>();
    public String getString(String key, String fallback) { return data.getOrDefault(key, fallback); }
    public Editor edit() { return new Editor(); }
    public final class Editor {
        private final java.util.Map<String, String> updates = new java.util.HashMap<>();
        public Editor putString(String key, String value) { updates.put(key, value); return this; }
        public Editor remove(String key) { updates.put(key, null); return this; }
        public void apply() { updates.forEach((k, v) -> { if (v == null) data.remove(k); else data.put(k, v); }); }
    }
}
