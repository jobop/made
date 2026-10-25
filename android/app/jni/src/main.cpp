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
#include "made_layout.hpp"
#include "made_sprite_cache.hpp"

#if defined(MADE_USE_FREETYPE)
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstdio>
#include <cstring>
#include <string>
#endif

// aligned_alloc 自 Android API 28 才有，我们 minSdk 21，用 posix_memalign 补一个。
#include <stdlib.h>
extern "C" void *aligned_alloc(size_t alignment, size_t size) {
    void *p = nullptr;
    return posix_memalign(&p, alignment, size) == 0 ? p : nullptr;
}

using esp_brookesia::apps::VibeCoding;

// 跨平台调试日志通道：安卓上裸 printf 不进 logcat，UI 层通过 made_dbg 走
// SDL_Log 输出；其他平台由 UI 层的 weak 定义回退到 printf。
extern "C" void made_dbg(const char *s) { SDL_Log("made: %s", s); }

// LVGL 的 SDL 窗口内部有一个 5ms 定时器 sdl_event_handler 自己 SDL_PollEvent
// 消费鼠标/键盘/窗口事件。所以主循环绝不能再用 SDL_PollEvent 抢事件（否则
// 鼠标/触摸被我们取走，LVGL 收不到，按钮点不动）。这里用 SDL_AddEventWatch
// 只「观察」事件（不消费），捕获退出与运行期尺寸变化。
static bool g_sdl_quit = false;
static int sdlEventWatch(void * /*userdata*/, SDL_Event *ev)
{
    if (ev->type == SDL_QUIT) {
        g_sdl_quit = true;
    }
    if (ev->type == SDL_WINDOWEVENT &&
        (ev->window.event == SDL_WINDOWEVENT_RESIZED ||
         ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) &&
        ev->window.data1 > 0 && ev->window.data2 > 0) {
        g_made_screen_w = ev->window.data1;
        g_made_screen_h = ev->window.data2;
    }
    return 0;
}

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

#if defined(MADE_USE_FREETYPE)
// 把 APK assets/fonts 下的 TTF 释放到应用内部存储。FreeType 走 stdio fopen，
// 读不到 APK 里的 asset，所以先落地成真实文件，再把目录交给 made_fonts_set_dir。
// 返回释放后的目录；任一字体缺失/写失败都返回空串（交由上层 fail-fast）。
static std::string madeExtractFonts()
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) {
        SDL_Log("made: fonts: no jni env/activity");
        return "";
    }
    jclass cls = env->GetObjectClass(activity);
    jmethodID getAssets = env->GetMethodID(
        cls, "getAssets", "()Landroid/content/res/AssetManager;");
    jobject amObj = getAssets ? env->CallObjectMethod(activity, getAssets) : nullptr;
    if (!amObj) {
        SDL_Log("made: fonts: activity.getAssets() failed");
        return "";
    }
    AAssetManager *am = AAssetManager_fromJava(env, amObj);
    if (!am) {
        SDL_Log("made: fonts: AAssetManager_fromJava failed");
        return "";
    }

    const char *base = SDL_AndroidGetInternalStoragePath();
    if (!base) {
        SDL_Log("made: fonts: no internal storage path");
        return "";
    }
    std::string dir = std::string(base) + "/fonts";
    mkdir(dir.c_str(), 0755);

    static const char *kFonts[] = {"puhui-common.ttf", "fontawesome.ttf"};
    for (const char *name : kFonts) {
        std::string assetPath = std::string("fonts/") + name;
        std::string outPath = dir + "/" + name;
        AAsset *a = AAssetManager_open(am, assetPath.c_str(), AASSET_MODE_BUFFER);
        if (!a) {
            SDL_Log("made: fonts: missing asset %s", assetPath.c_str());
            return "";
        }
        off_t len = AAsset_getLength(a);
        const void *buf = AAsset_getBuffer(a);
        bool ok = false;
        FILE *f = fopen(outPath.c_str(), "wb");
        if (f) {
            ok = (len > 0) &&
                 (fwrite(buf, 1, (size_t)len, f) == (size_t)len);
            fclose(f);
        }
        AAsset_close(a);
        if (!ok) {
            SDL_Log("made: fonts: write %s failed", outPath.c_str());
            return "";
        }
        SDL_Log("made: fonts: extracted %s (%lld bytes)", outPath.c_str(),
                (long long)len);
    }
    return dir;
}
#endif

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_AddEventWatch(sdlEventWatch, nullptr);

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
    // 来源依次尝试：JNI getRealSize（原生、最权威）> SDL_GetCurrentDisplayMode >
    // SDL_GetDisplayBounds。拿不到就直接失败退出，绝不使用编译期默认值静默兜底
    // （否则 UI 会按错误尺寸渲染，问题被掩盖）。
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
        }
    }
    if (real_w <= 0 || real_h <= 0) {
        SDL_Log("FATAL: made cannot determine physical screen size; refusing to start "
                "(JNI/SDL display queries all returned invalid)");
        SDL_Quit();
        return 1;
    }
    // 竖屏统一为 w<=h（UI 按竖屏设计）。
    if (real_w > real_h) { int t = real_w; real_w = real_h; real_h = t; }

    // 画布与 UI 布局共用同一份运行时分辨率，避免按错误短边布局。
    g_made_screen_w = real_w;
    g_made_screen_h = real_h;

    lv_init();

    // ---- 宿主环境接线（真机由 BSP/驱动承担的部分）----
    // 1) 网络：Android 的 Wi-Fi 由系统管理，应用层只关心「有网可用」。
    //    vibe_wifi 的宿主桩用 MADE_SIM_WIFI 门控「已连接」，这里打开它。
    setenv("MADE_SIM_WIFI", "1", 1);
    // 2) 存储：精灵素材缓存需要可写目录。真机挂在 /spiffs（SPIFFS），
    //    安卓映射到应用私有目录下的 spiffs/。
    if (const char *base = SDL_AndroidGetInternalStoragePath()) {
        std::string cache_dir = std::string(base) + "/spiffs";
        mkdir(cache_dir.c_str(), 0755);
        made_sprite_cache::set_mount(cache_dir);
        SDL_Log("made: sprite cache mount %s", cache_dir.c_str());
    }

#if defined(MADE_USE_FREETYPE)
    // 矢量字体必须在 UI 构建（startOnDisplay → made_fonts_init）前就位；
    // 释放失败直接拒绝启动，不静默回落到默认字体掩盖打包问题。
    std::string fontDir = madeExtractFonts();
    if (fontDir.empty()) {
        SDL_Log("FATAL: made font assets could not be prepared; refusing to start");
        SDL_Quit();
        return 1;
    }
    made_fonts_set_dir(fontDir.c_str());
#endif

    lv_display_t *disp = lv_sdl_window_create(real_w, real_h);
    if (!disp) { SDL_Log("lv_sdl_window_create failed"); return 1; }
    SDL_Log("made: canvas created %dx%d", real_w, real_h);

    // 保险：再同步一次分辨率（SDL 初始 resize 事件可能微调，如扣系统栏）。
    lv_display_set_resolution(disp, real_w, real_h);

    lv_display_set_render_mode(disp, LV_DISPLAY_RENDER_MODE_FULL);
    lv_sdl_mouse_create();   // 安卓触屏 → LVGL 指针设备
    // 键盘 indev + 默认 group：宿主侧 adb input text / 外接蓝牙键盘可直接
    // 输入（事件经 SDL KEYDOWN → group 焦点 textarea）。触屏用户无感。
    if (!lv_group_get_default()) lv_group_set_default(lv_group_create());
    lv_indev_t *kb_indev = lv_sdl_keyboard_create();
    if (kb_indev && lv_group_get_default()) lv_indev_set_group(kb_indev, lv_group_get_default());

    if (!VibeCoding::startOnDisplay()) {
        SDL_Log("VibeCoding::startOnDisplay failed");
        return 1;
    }

    SDL_Log("made: UI started on %dx%d", g_made_screen_w, g_made_screen_h);

    while (!g_sdl_quit) {
        // 仅把系统事件泵入队列，不消费：鼠标/触摸/键盘由 LVGL 的
        // sdl_event_handler 自行 PollEvent 处理；退出与 resize 由上面的
        // event watch 观察。若这里再 PollEvent 会抢占事件导致按钮点不动。
        SDL_PumpEvents();
        lv_tick_inc(16);
        lv_timer_handler();
        SDL_Delay(16);
    }

    SDL_Quit();
    return 0;
}
