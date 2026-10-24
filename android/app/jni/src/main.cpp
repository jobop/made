// android/app/src/main/jni/android_main.cpp
//
// 安卓端 native 入口。SDL2-Android 会把 main() 当作 SDL_main 加载。
// 与 sim/sim_main.cpp 同一套启动序列，但去掉截图/手势自动化等调试代码，
// 只保留「开 SDL 窗口 → 起 LVGL → 跑 made UI → 主循环」。
//
// 应用层 VibeCoding 一行不改。

#include "lvgl.h"
#include <SDL2/SDL.h>
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

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }

    lv_init();
    int lv_w = 480, lv_h = 1055;
    SDL_Log("lvgl logical %dx%d", lv_w, lv_h);
    lv_display_t *disp = lv_sdl_window_create(lv_w, lv_h);
    if (!disp) { SDL_Log("lv_sdl_window_create failed"); return 1; }

    lv_display_set_render_mode(disp, LV_DISPLAY_RENDER_MODE_FULL);
    lv_sdl_mouse_create();   // 安卓触屏 → LVGL 指针设备

    if (!VibeCoding::startOnDisplay()) {
        SDL_Log("VibeCoding::startOnDisplay failed");
        return 1;
    }

    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = true;
        }
        lv_tick_inc(16);
        lv_timer_handler();
        SDL_Delay(16);
    }

    SDL_Quit();
    return 0;
}
