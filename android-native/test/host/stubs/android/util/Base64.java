package android.util;
public final class Base64 {
    public static final int URL_SAFE = 8, NO_WRAP = 2, NO_PADDING = 1;
    public static byte[] decode(String text, int flags) { return java.util.Base64.getUrlDecoder().decode(text); }
}
