// android/app/src/main/jni/android_main.cpp
//
// 安卓端 native 入口。SDL2-Android 会把 main() 当作 SDL_main 加载。
// 与 sim/sim_main.cpp 同一套启动序列，区别只在分辨率：
//
// 不同手机/屏幕的物理像素不同，不能硬编码。这里在创建 SDL 窗口 *之前* 运行时探测
// 真实物理分辨率（JNI getRealSize > SDL_GetCurrentDisplayMode > DisplayBounds >
// 编译期兜底），再用它一次性创建画布与 UI，保证「画布分辨率 == 物理尺寸」。
// 注意：SDL_GetWindowSize 在安卓上只会回显 SDL_CreateWindow 传入的请求值，
// 不能用来探测；而事后 lv_display_set_resolution 不会重排已创建的控件，
// 所以必须在 startOnDisplay() 之前就拿到真实尺寸。
//
// 应用层 VibeCoding 一行不改。

#include "lvgl.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#include <jni.h>
#include "bsp/display.h"
#include "esp_brookesia_app_vibe.hpp"

// aligned_alloc 自 Android API 28 才有，我们 minSdk 21，用 posix_memalign 补一个。
#include <stdlib.h>
extern "C" void *aligned_alloc(size_t alignment, size_t size) {
    void *p = nullptr;
    return posix_memalign(&p, alignment, size) == 0 ? p : nullptr;
}

using esp_brookesia::apps::VibeCoding;

// 最权威的物理尺寸来源：直接调 Android 原生 API，不经过 SDL，避免被游戏助手 /
// 兼容模式（会给 SDL 一个被改写的显示模式）影响。
//   activity.getWindowManager().getDefaultDisplay().getRealSize(Point)
// 成功返回 true 并写入 w/h（物理全屏像素，含状态栏/导航栏区域）。
static bool jniGetRealSize(int &outW, int &outH)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) {
        SDL_Log("made: jni env/activity null (env=%p act=%p)", (void *)env,
                activity ? (void *)activity : nullptr);
        return false;
    }

    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getWM = env->GetMethodID(activityClass, "getWindowManager",
                                      "()Landroid/view/WindowManager;");
    jobject wm = getWM ? env->CallObjectMethod(activity, getWM) : nullptr;
    jclass wmClass = wm ? env->GetObjectClass(wm) : nullptr;
    jmethodID getDisplay = wmClass
        ? env->GetMethodID(wmClass, "getDefaultDisplay", "()Landroid/view/Display;")
        : nullptr;
    jobject display = getDisplay ? env->CallObjectMethod(wm, getDisplay) : nullptr;
    jclass displayClass = display ? env->GetObjectClass(display) : nullptr;
    jclass pointClass = env->FindClass("android/graphics/Point");
    jmethodID pointCtor = pointClass ? env->GetMethodID(pointClass, "<init>", "()V") : nullptr;
    jobject point = (pointClass && pointCtor) ? env->NewObject(pointClass, pointCtor) : nullptr;
    jmethodID getRealSize = displayClass
        ? env->GetMethodID(displayClass, "getRealSize", "(Landroid/graphics/Point;)V")
        : nullptr;

    bool ok = false;
    if (display && point && getRealSize) {
        env->CallVoidMethod(display, getRealSize, point);
        if (env->ExceptionCheck()) env->ExceptionClear();
        jfieldID fx = env->GetFieldID(pointClass, "x", "I");
        jfieldID fy = env->GetFieldID(pointClass, "y", "I");
        int x = fx ? env->GetIntField(point, fx) : 0;
        int y = fy ? env->GetIntField(point, fy) : 0;
        if (x > 0 && y > 0) { outW = x; outH = y; ok = true; }
    }

    if (env->ExceptionCheck()) env->ExceptionClear();
    if (point) env->DeleteLocalRef(point);
    if (display) env->DeleteLocalRef(display);
    if (wm) env->DeleteLocalRef(wm);
    return ok;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }

    // 诊断：SDL 视角下的显示器信息（真机曾被游戏助手/兼容模式干扰）。
    int n = SDL_GetNumVideoDisplays();
    SDL_Log("made: SDL video displays=%d", n);
    if (n > 0) {
        SDL_DisplayMode dm;
        SDL_Rect b;
        if (SDL_GetCurrentDisplayMode(0, &dm) == 0)
            SDL_Log("made: CurrentDisplayMode %dx%d fmt=%d rate=%d",
                    dm.w, dm.h, dm.format, dm.refresh_rate);
        if (SDL_GetDisplayBounds(0, &b) == 0)
            SDL_Log("made: DisplayBounds x=%d y=%d w=%d h=%d", b.x, b.y, b.w, b.h);
    }

    // 先探测真实物理尺寸，再用它创建窗口——SDL_CreateWindow 传入的尺寸会被
    // SDL2-Android 当作渲染目标尺寸，事后 set_resolution 不一定能让 surface 回到
    // 全屏；所以必须在 create 之前拿到真实尺寸。
    // 优先级：JNI getRealSize（原生、最权威）> SDL_GetCurrentDisplayMode >
    // SDL_GetDisplayBounds > 编译期兜底。
    int real_w = 0, real_h = 0;
    int jw = 0, jh = 0;
    if (jniGetRealSize(jw, jh)) {
        real_w = jw; real_h = jh;
        SDL_Log("made: use JNI getRealSize %dx%d", real_w, real_h);
    } else {
        SDL_DisplayMode dm;
        SDL_Rect b;
        if (n > 0 && SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
            real_w = dm.w; real_h = dm.h;
            SDL_Log("made: use CurrentDisplayMode %dx%d", real_w, real_h);
        } else if (n > 0 && SDL_GetDisplayBounds(0, &b) == 0 && b.w > 0 && b.h > 0) {
            real_w = b.w; real_h = b.h;
            SDL_Log("made: use DisplayBounds %dx%d", real_w, real_h);
        } else {
            real_w = BSP_LCD_H_RES; real_h = BSP_LCD_V_RES;
            SDL_Log("made: display query failed, fallback compile-time %dx%d", real_w, real_h);
        }
    }
    // 竖屏统一为 w<=h（UI 按竖屏设计）。
    if (real_w > real_h) { int t = real_w; real_w = real_h; real_h = t; }

    // 画布与 UI 布局共用同一份运行时分辨率，避免按错误短边布局。
    g_made_screen_w = real_w;
    g_made_screen_h = real_h;

    lv_init();
    lv_display_t *disp = lv_sdl_window_create(real_w, real_h);
    if (!disp) { SDL_Log("lv_sdl_window_create failed"); return 1; }
    SDL_Log("made: canvas created %dx%d", real_w, real_h);

    // 保险：再同步一次分辨率（SDL 初始 resize 事件可能微调，如扣系统栏）。
    lv_display_set_resolution(disp, real_w, real_h);

    lv_display_set_render_mode(disp, LV_DISPLAY_RENDER_MODE_FULL);
    lv_sdl_mouse_create();   // 安卓触屏 → LVGL 指针设备

    if (!VibeCoding::startOnDisplay()) {
        SDL_Log("VibeCoding::startOnDisplay failed");
        return 1;
    }

    SDL_Log("made: UI started on %dx%d", g_made_screen_w, g_made_screen_h);

    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = true;
            // 运行期尺寸变化（横竖屏切换/折叠）：同步画布与布局变量。
            if (ev.type == SDL_WINDOWEVENT &&
                (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                 ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) &&
                ev.window.data1 > 0 && ev.window.data2 > 0) {
                g_made_screen_w = ev.window.data1;
                g_made_screen_h = ev.window.data2;
                SDL_Log("made: window resized to %dx%d (re-layout needed)",
                        g_made_screen_w, g_made_screen_h);
            }
        }
        lv_tick_inc(16);
        lv_timer_handler();
        SDL_Delay(16);
    }

    SDL_Quit();
    return 0;
}
