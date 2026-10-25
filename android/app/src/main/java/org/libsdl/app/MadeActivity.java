package org.libsdl.app;

import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;

/**
 * made 自定义 Activity，作为应用入口（后续音频/权限/沉浸式等安卓定制都挂这里）。
 *
 * 屏幕分辨率不需要在 Java 层传递：native 端 main.cpp 会通过 JNI 直接调用
 * getWindowManager().getDefaultDisplay().getRealSize() 运行时获取物理分辨率。
 *
 * 但必须让 SDL 窗口真正占满物理全屏：默认情况下系统会把窗口 inset 到
 * 状态栏/导航栏之间（顶部 ~128px、底部 ~63px），此时渲染按物理全屏 1:1 输出、
 * 触摸却按被压缩的窗口区域换算回逻辑分辨率，两套映射不一致，表现为
 * 「点到的控件比看到的控件整体偏移」。这里用 sticky immersive 隐藏系统栏，
 * 并用 FLAG_LAYOUT_NO_LIMITS 让内容延伸到系统栏后方，使 drawable 区域 ==
 * 物理分辨率，渲染与触摸坐标系严格 1:1 对齐。
 */
public class MadeActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // 允许内容绘制到打孔/刘海（cutout）区域，否则竖屏下系统会强制把窗口整体
        // 下推一个状态栏高度（顶部黑边），导致 GL 渲染从物理 0 开始、触摸坐标却
        // 从 inset 后的窗口区换算，两套映射不一致而点不准。
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            if (Build.VERSION.SDK_INT >= 30) {
                lp.layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
            } else {
                lp.layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            }
            getWindow().setAttributes(lp);
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS);
        applyImmersive();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyImmersive();
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyImmersive();
    }

    private void applyImmersive() {
        // 从屏幕边缘滑出系统栏时半透明短暂显示、稍后自动收回（sticky），不抢焦点。
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }
}
