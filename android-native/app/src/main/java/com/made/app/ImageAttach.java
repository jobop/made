package com.made.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.media.ExifInterface;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.database.Cursor;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;

/**
 * 系统图片选择器返回的 Uri → 可上传的 JPEG。
 * 只用 android framework（不引 Glide/Coil）：下采样、EXIF 转正、透明区铺白底、
 * 循环压到 1.4MB 以内，手机原图不会直接打爆内存或上行带宽。
 */
public final class ImageAttach {
    public static final int MAX_SIDE = 1600;
    public static final int MAX_UPLOAD_BYTES = 1400 * 1024;

    public static final class Picked {
        public byte[] jpeg;
        public String name = "photo.jpg";
        public Bitmap preview;
    }

    private ImageAttach() { }

    public static Picked load(Context context, Uri uri) throws Exception {
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        try (InputStream in = context.getContentResolver().openInputStream(uri)) {
            if (in == null) throw new Exception("打不开图片");
            BitmapFactory.decodeStream(in, null, bounds);
        }
        int width = bounds.outWidth, height = bounds.outHeight;
        if (width <= 0 || height <= 0) throw new Exception("不是有效的图片");

        int sample = 1;
        while (Math.max(width, height) / sample > MAX_SIDE * 2) sample *= 2;
        BitmapFactory.Options decode = new BitmapFactory.Options();
        decode.inSampleSize = sample;
        Bitmap src;
        try (InputStream in = context.getContentResolver().openInputStream(uri)) {
            if (in == null) throw new Exception("打不开图片");
            src = BitmapFactory.decodeStream(in, null, decode);
        }
        if (src == null) throw new Exception("图片解码失败");

        int rotation = rotationOf(context, uri);
        if (rotation != 0) {
            Matrix matrix = new Matrix();
            matrix.postRotate(rotation);
            Bitmap rotated = Bitmap.createBitmap(src, 0, 0, src.getWidth(), src.getHeight(), matrix, true);
            if (rotated != src) { src.recycle(); src = rotated; }
        }
        int longest = Math.max(src.getWidth(), src.getHeight());
        if (longest > MAX_SIDE) {
            float scale = (float) MAX_SIDE / longest;
            Bitmap scaled = Bitmap.createScaledBitmap(src,
                    Math.max(1, Math.round(src.getWidth() * scale)),
                    Math.max(1, Math.round(src.getHeight() * scale)), true);
            if (scaled != src) { src.recycle(); src = scaled; }
        }

        // PNG/WebP 的透明区直接压成 JPEG 会变黑，先铺白底。
        Bitmap flat = Bitmap.createBitmap(src.getWidth(), src.getHeight(), Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(flat);
        canvas.drawColor(Color.WHITE);
        canvas.drawBitmap(src, 0, 0, null);
        src.recycle();
        src = flat;

        int quality = 82;
        byte[] jpeg = compress(src, quality);
        while (jpeg.length > MAX_UPLOAD_BYTES && quality > 50) {
            quality -= 12;
            jpeg = compress(src, quality);
        }
        if (jpeg.length > MAX_UPLOAD_BYTES) {
            Bitmap smaller = Bitmap.createScaledBitmap(src,
                    Math.max(1, src.getWidth() * 2 / 3), Math.max(1, src.getHeight() * 2 / 3), true);
            jpeg = compress(smaller, 70);
            if (smaller != src) smaller.recycle();
        }

        Picked picked = new Picked();
        picked.jpeg = jpeg;
        picked.name = nameOf(context, uri);
        float thumbScale = Math.min(1f, 240f / Math.max(src.getWidth(), src.getHeight()));
        picked.preview = Bitmap.createScaledBitmap(src,
                Math.max(1, Math.round(src.getWidth() * thumbScale)),
                Math.max(1, Math.round(src.getHeight() * thumbScale)), true);
        if (picked.preview != src) src.recycle();
        return picked;
    }

    /** 任务卡回显：按最长边下采样，避免整张 1600px 图进 UI 内存。 */
    public static Bitmap decodePreview(byte[] bytes, int maxSide) {
        if (bytes == null || bytes.length == 0) return null;
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(bytes, 0, bytes.length, bounds);
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null;
            int sample = 1;
            while (Math.max(bounds.outWidth, bounds.outHeight) / sample > maxSide * 2) sample *= 2;
            BitmapFactory.Options decode = new BitmapFactory.Options();
            decode.inSampleSize = sample;
            return BitmapFactory.decodeByteArray(bytes, 0, bytes.length, decode);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static byte[] compress(Bitmap bitmap, int quality) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        bitmap.compress(Bitmap.CompressFormat.JPEG, quality, out);
        return out.toByteArray();
    }

    private static int rotationOf(Context context, Uri uri) {
        try (InputStream in = context.getContentResolver().openInputStream(uri)) {
            if (in == null) return 0;
            ExifInterface exif = new ExifInterface(in);
            switch (exif.getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)) {
                case ExifInterface.ORIENTATION_ROTATE_90: return 90;
                case ExifInterface.ORIENTATION_ROTATE_180: return 180;
                case ExifInterface.ORIENTATION_ROTATE_270: return 270;
                default: return 0;
            }
        } catch (Exception ignored) {
            return 0;
        }
    }

    private static String nameOf(Context context, Uri uri) {
        String name = null;
        try (Cursor cursor = context.getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) name = cursor.getString(idx);
            }
        } catch (Exception ignored) { }
        if (name == null || name.isEmpty()) name = uri.getLastPathSegment();
        if (name == null || name.isEmpty()) name = "photo.jpg";
        StringBuilder clean = new StringBuilder();
        for (char ch : name.toCharArray()) {
            clean.append(ch == '/' || ch == '\\' || ch < 32 ? '_' : ch);
        }
        String result = clean.toString();
        if (result.length() > 80) result = result.substring(result.length() - 80);
        String lower = result.toLowerCase();
        if (!lower.endsWith(".jpg") && !lower.endsWith(".jpeg")) result += ".jpg";
        return result;
    }
}
