package com.made.app;

import android.content.Context;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.MotionEvent;
import android.view.View;
import android.widget.TextView;

/**
 * 设计 token 与视图工厂。
 * 8px 基线；暖灰底 + 品牌橙主色；仅动画 transform（按压缩放 ~120ms）。
 */
public final class Ui {
    // ---- 色板 ----
    public static final int BG = 0xFFF7F5F2;          // 页面底
    public static final int SURFACE = 0xFFFFFFFF;     // 卡片
    public static final int INK = 0xFF211E1B;         // 主文字
    public static final int SUB = 0xFF8B857D;         // 次文字
    public static final int LINE = 0xFFECE7E0;        // 分隔线/描边
    public static final int PRIMARY = 0xFFFF6C3E;     // 品牌橙
    public static final int PRIMARY_DEEP = 0xFFD84E1D;// 主色按压态
    public static final int PRIMARY_SOFT = 0xFFFFEFE7;// 主色浅底
    public static final int OK = 0xFF2E9E63;          // 成功
    public static final int WARN = 0xFFE0982E;        // 等待
    public static final int ERR = 0xFFD64530;         // 错误
    public static final int SOFT = 0xFFF2EFEA;        // 输入框底

    private Ui() { }

    public static int dp(Context c, float v) {
        return Math.round(v * c.getResources().getDisplayMetrics().density);
    }

    public static GradientDrawable fill(Context c, int color, float radiusDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(c, radiusDp));
        return d;
    }

    public static GradientDrawable card(Context c, int color, float radiusDp,
                                        int strokeColor, float strokeDp) {
        GradientDrawable d = fill(c, color, radiusDp);
        if (strokeColor != 0) d.setStroke(dp(c, strokeDp), strokeColor);
        return d;
    }

    public static GradientDrawable circle(int color) {
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.OVAL);
        d.setColor(color);
        return d;
    }

    /** 按压缩放微交互（只动 scale；不消费事件，点击照常）。 */
    public static void press(View v) { press(v, 0.96f); }

    public static void press(View v, float scale) {
        v.setOnTouchListener((view, e) -> {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    view.animate().scaleX(scale).scaleY(scale).setDuration(90).start();
                    break;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    view.animate().scaleX(1f).scaleY(1f).setDuration(120).start();
                    break;
            }
            return false;
        });
    }

    public static TextView text(Context c, String s, float sp, int color, boolean bold) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    /** 胶囊形文字按钮（主色/浅色两种）。 */
    public static TextView pillButton(Context c, String label, boolean primary) {
        TextView t = text(c, label, 15, primary ? 0xFFFFFFFF : PRIMARY, true);
        t.setGravity(android.view.Gravity.CENTER);
        t.setBackground(primary ? fill(c, PRIMARY, 24) : card(c, PRIMARY_SOFT, 24, PRIMARY, 1));
        t.setPadding(dp(c, 20), dp(c, 11), dp(c, 20), dp(c, 11));
        press(t);
        return t;
    }
}
