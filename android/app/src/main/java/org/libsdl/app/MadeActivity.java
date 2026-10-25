package org.libsdl.app;

import android.os.Bundle;

/**
 * made 自定义 Activity，作为应用入口（后续音频/权限/沉浸式等安卓定制都挂这里）。
 *
 * 屏幕分辨率不需要在 Java 层传递：native 端 main.cpp 会通过 JNI 直接调用
 * getWindowManager().getDefaultDisplay().getRealSize() 运行时获取物理分辨率，
 * 因此这里保持 SDLActivity 默认行为即可。
 */
public class MadeActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
    }
}
