package org.json;

// Host-only JSON compatibility shim backed by the Android SDK toolchain's Gson.
public final class JSONObject {
    private final com.google.gson.JsonObject object;
    public JSONObject() { object = new com.google.gson.JsonObject(); }
    public JSONObject(String json) { object = com.google.gson.JsonParser.parseString(json).getAsJsonObject(); }
    public String optString(String key) { return optString(key, ""); }
    public String optString(String key, String fallback) {
        return object.has(key) && !object.get(key).isJsonNull() ? object.get(key).getAsString() : fallback;
    }
    public long optLong(String key, long fallback) { return object.has(key) ? object.get(key).getAsLong() : fallback; }
    public JSONObject put(String key, String value) { object.addProperty(key, value); return this; }
    public String toString() { return object.toString(); }
}
