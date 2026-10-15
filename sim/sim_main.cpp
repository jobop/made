// sim/sim_main.cpp — 码得 UI 模拟器主程序（host 运行）。
//
// 用法：
//   sim                                  # 开窗口跑真实 UI，鼠标=触摸
//   sim --shot out                       # 渲染稳定后截图 out.bmp/.png（AI 自查布局用）
//   sim --shot out --swipe 120 300 120 60 --wait 30   # 先模拟手势再截图
//   sim --tap x y ...                    # 模拟点击序列
//   MADE_SIM_PAIRING=paired / MADE_SIM_WIFI=1 可切换桩状态。
#include "lvgl.h"
#include <SDL2/SDL.h>
#include "esp_brookesia_app_vibe.hpp"
#include "made_sprite.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using esp_brookesia::apps::VibeCoding;

namespace {

// MADE_SIM_SPRITE_DIR=<dir>：独立精灵测试模式——不走 App，直接在活动屏幕上
// 建 made_sprite 舞台并加载 <dir>/manifest.json 分层素材，验证真实素材的
// 解码/烘焙/帧轮播渲染（AI 自查花屏/错位用）。
void captureShot(const std::string &path, lv_display_t *disp);

std::string read_file(const std::string &path)
{    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return std::string();
    std::string json;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) json.append(buf, n);
    fclose(fp);
    return json;
}

int run_sprite_test()
{
    const char *dir = getenv("MADE_SIM_SPRITE_DIR");
    if (!dir || !*dir) return 0;
    std::string manifest = read_file(std::string(dir) + "/manifest.json");
    if (manifest.empty()) {
        fprintf(stderr, "sim: no manifest.json in %s\n", dir);
        return 1;
    }
    lv_display_t *disp = lv_display_get_default();
    lv_obj_t *stage = made_sprite::create(lv_screen_active());
    std::string base = dir;
    if (base.empty() || base.back() != '/') base += '/';
    if (!stage || !made_sprite::applyArtwork(stage, manifest, base)) {
        fprintf(stderr, "sim: sprite test apply failed\n");
        return 1;
    }
    uint32_t elapsed = 0;
    for (int i = 0; i < 400; ++i) {   // ~13s：留足后台烘焙时间
        SDL_PumpEvents();
        lv_tick_inc(33);
        lv_timer_handler();
        SDL_Delay(30);
        elapsed += 33;
        made_sprite::update(stage, elapsed, made_sprite::Mood::kIdle);
    }

    // MADE_SIM_MOODS="idle,running,..."：逐个情绪渲染 ~3s 后截图
    // <prefix>_<序号>_<mood>.bmp——情绪表切帧的离线验证（配合
    // MADE_SIM_SPRITE_DIR / MADE_SIM_SHOT_PREFIX 使用）。
    if (const char *moods = getenv("MADE_SIM_MOODS"); moods && *moods) {
        const char *prefix = getenv("MADE_SIM_SHOT_PREFIX");
        std::string tag = prefix && *prefix ? prefix : "/tmp/mood";
        struct Named { const char *name; int mood; };
        static const Named kNamed[] = {
            {"idle", made_sprite::Mood::kIdle},
            {"talking", made_sprite::Mood::kTalking},
            {"running", made_sprite::Mood::kRunning},
            {"waiting", made_sprite::Mood::kWaitingConfirm},
            {"success", made_sprite::Mood::kSuccess},
            {"sad", made_sprite::Mood::kSad},
            {"offline", made_sprite::Mood::kOffline},
        };
        std::string list = moods;
        int index = 0;
        for (size_t start = 0; start <= list.size();) {
            size_t comma = list.find(',', start);
            std::string name = list.substr(start,
                comma == std::string::npos ? std::string::npos : comma - start);
            if (name.empty()) break;
            const Named *found = nullptr;
            for (const auto &entry : kNamed)
                if (name == entry.name) { found = &entry; break; }
            if (!found) {
                fprintf(stderr, "sim: unknown mood '%s'\n", name.c_str());
                return 1;
            }
            for (int i = 0; i < 90; ++i) {   // ~3s：跨过眨眼周期取稳定帧
                SDL_PumpEvents(); lv_tick_inc(33); lv_timer_handler(); SDL_Delay(30);
                elapsed += 33;
                made_sprite::update(stage, elapsed, found->mood);
            }
            captureShot(tag + "_" + std::to_string(index++) + "_" + name + ".bmp", disp);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    // MADE_SIM_SWIPE_DEMO=1：左右滑跟手 + 真翻页演示。侧舞台（第二套素材）
    // 停在屏外 ±1 屏宽，拖动平移 screen 时从屏边滑入视野；松手晋升（换指针）。
    // 每步截图，供检查滑动过渡与翻页预览。
    if (getenv("MADE_SIM_SWIPE_DEMO")) {
        const char *prefix = getenv("MADE_SIM_SHOT_PREFIX");
        std::string tag = prefix && *prefix ? prefix : "/tmp/swipe";
        lv_obj_t *screen = lv_screen_active();
        const int w = 240;
        lv_obj_add_flag(screen, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_set_ext_draw_size(screen, w);
        lv_obj_refresh_ext_draw_size(screen);  // set 只注册回调，刷新一次才写入
        auto pump = [&]() {
            SDL_PumpEvents(); lv_tick_inc(33); lv_timer_handler(); SDL_Delay(30);
            elapsed += 33;
            made_sprite::update(stage, elapsed, made_sprite::Mood::kIdle);
        };
        auto pan = [&](int dx, const std::string &path) {
            lv_obj_set_x(screen, dx);
            pump();
            pump();
            captureShot(path, disp);
        };
        auto stage2 = [&](lv_obj_t *st, const char *d) {
            std::string m = read_file(std::string(d) + "/manifest.json");
            std::string b = d;
            if (b.empty() || b.back() != '/') b += '/';
            made_sprite::applyArtwork(st, m, b);
            for (int i = 0; i < 10; ++i) pump();
        };
        const char *dir2 = getenv("MADE_SIM_SPRITE_DIR2");
        // 右侧挂第二套素材：向左拖时它从屏边滑入（真翻页预览）。
        lv_obj_t *neighbor = made_sprite::create(screen);
        lv_obj_set_x(neighbor, w);
        if (dir2 && *dir2) stage2(neighbor, dir2);
        pan(0, tag + "_00.bmp");                       // 起点：素材完整可见
        for (int k = 1; k <= 6; ++k) pan(-w * k / 6, tag + "_a" + std::to_string(k) + ".bmp");
        // 松手晋升：侧舞台移到正位，旧主舞台撤装停靠屏外，指针互换。
        lv_obj_set_x(neighbor, 0);
        made_sprite::clearArtwork(stage);
        lv_obj_set_x(stage, w * 2);
        std::swap(stage, neighbor);
        lv_obj_set_x(screen, 0);
        for (int i = 0; i < 3; ++i) pump();
        captureShot(tag + "_b0.bmp", disp);
        // 反向演示：旧素材（第一套）挂到屏外左侧，向右拖时从左边滑入。
        if (dir2 && *dir2) {
            stage2(neighbor, dir);                     // neighbor 现在是旧主舞台
            lv_obj_set_x(neighbor, -w);
            for (int k = 1; k <= 6; ++k) pan(w * k / 6, tag + "_b" + std::to_string(k) + ".bmp");
            lv_obj_set_x(neighbor, 0);
            made_sprite::clearArtwork(stage);
            lv_obj_set_x(stage, -w * 2);
            std::swap(stage, neighbor);
            lv_obj_set_x(screen, 0);
            for (int i = 0; i < 10; ++i) pump();
            captureShot(tag + "_c1.bmp", disp);
        }
    }
    return 0;
}

struct SimAction {
    enum Type { Tap, Swipe, Wait } type = Wait;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int frames = 0;
};

struct Args {
    std::vector<SimAction> actions;
    std::string shot_path;
    int total_frames = 90;
    int settle_frames = 60; // 手势做完后等 UI 稳定再截图
};

Args parseArgs(int argc, char **argv)
{
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shot" && i + 1 < argc) {
            args.shot_path = argv[++i];
        } else if (arg == "--frames" && i + 1 < argc) {
            args.total_frames = atoi(argv[++i]);
        } else if (arg == "--tap" && i + 3 < argc) {
            SimAction a; a.type = SimAction::Tap;
            a.x1 = atoi(argv[++i]); a.y1 = atoi(argv[++i]);
            const int hold = atoi(argv[++i]);
            a.frames = hold > 0 ? hold : 6;
            args.actions.push_back(a);
        } else if (arg == "--swipe" && i + 5 < argc) {
            SimAction a; a.type = SimAction::Swipe;
            a.x1 = atoi(argv[++i]); a.y1 = atoi(argv[++i]);
            a.x2 = atoi(argv[++i]); a.y2 = atoi(argv[++i]);
            a.frames = 14;
            args.actions.push_back(a);
        } else if (arg == "--wait" && i + 1 < argc) {
            SimAction a; a.type = SimAction::Wait;
            a.frames = atoi(argv[++i]);
            args.actions.push_back(a);
        }
    }
    return args;
}

void pushMouse(SDL_Window *window, uint32_t type, float x, float y)
{
    SDL_Event ev {};
    ev.type = type;
    ev.button.windowID = SDL_GetWindowID(window);
    ev.button.which = 0;
    ev.button.clicks = 1;
    ev.button.x = x;
    ev.button.y = y;
    if (type == SDL_MOUSEBUTTONDOWN) {
        ev.button.state = SDL_PRESSED;
        ev.button.button = SDL_BUTTON_LEFT;
    } else if (type == SDL_MOUSEBUTTONUP) {
        ev.button.state = SDL_RELEASED;
        ev.button.button = SDL_BUTTON_LEFT;
    } else if (type == SDL_MOUSEMOTION) {
        ev.motion.state = SDL_BUTTON_LMASK;
    }
    SDL_PushEvent(&ev);
}

void pushHover(SDL_Window *window, float x, float y)
{
    SDL_Event ev {};
    ev.type = SDL_MOUSEMOTION;
    ev.motion.windowID = SDL_GetWindowID(window);
    ev.motion.which = 0;
    ev.motion.state = 0;   // 未按住：纯悬停移动
    ev.motion.x = x;
    ev.motion.y = y;
    SDL_PushEvent(&ev);
}

void runTap(const SimAction &a, SDL_Window *window, int &frame)
{
    // 先发一个未按住的 MOTION 把光标挪到位：SDL 鼠标设备没有位置事件时，
    // 部分按下事件会被 indev 丢弃（实测偶发 tap 失效）。
    pushHover(window, (float)a.x1, (float)a.y1);
    SDL_PumpEvents();
    pushMouse(window, SDL_MOUSEBUTTONDOWN, (float)a.x1, (float)a.y1);
    for (int i = 0; i < 3; ++i) { SDL_PumpEvents(); lv_tick_inc(33); lv_timer_handler(); SDL_Delay(30); ++frame; }
    pushMouse(window, SDL_MOUSEBUTTONUP, (float)a.x1, (float)a.y1);
    for (int i = 3; i < a.frames; ++i) { SDL_PumpEvents(); lv_tick_inc(33); lv_timer_handler(); SDL_Delay(30); ++frame; }
}

void runSwipe(const SimAction &a, SDL_Window *window, int &frame)
{
    // LVGL 手势识别要求「单周期速度 ≥3px 且累计位移 >50px」且途中会被清零；
    // 慢速长滑会卡在阈值下永远不触发（真机触采样率高无此问题）。
    // 模拟器用 6 步快速滑，保证每周期 ~37px 位移。
    const int steps = 6;
    pushMouse(window, SDL_MOUSEBUTTONDOWN, (float)a.x1, (float)a.y1);
    for (int i = 0; i <= steps; ++i) {
        const float t = (float)i / (float)steps;
        const float x = a.x1 + (a.x2 - a.x1) * t;
        const float y = a.y1 + (a.y2 - a.y1) * t;
        pushMouse(window, SDL_MOUSEMOTION, x, y);
        SDL_PumpEvents(); lv_tick_inc(33); lv_timer_handler(); SDL_Delay(30); ++frame;
    }
    pushMouse(window, SDL_MOUSEBUTTONUP, (float)a.x2, (float)a.y2);
    SDL_PumpEvents();
}

/* RGB565 帧缓冲 → 24 位 BMP（自底向上行序）。 */
void writeBmp(const std::string &path, const uint8_t *rgb565, int w, int h, uint32_t stride)
{
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp) { fprintf(stderr, "sim: cannot write %s\n", path.c_str()); return; }
    const int row = (w * 3 + 3) & ~3;
    const uint32_t data_size = (uint32_t)row * h;
    uint8_t header[54] = {0};
    header[0] = 'B'; header[1] = 'M';
    const uint32_t file_size = 54 + data_size;
    memcpy(header + 2, &file_size, 4);
    const uint32_t offset = 54; memcpy(header + 10, &offset, 4);
    const uint32_t dib = 40; memcpy(header + 14, &dib, 4);
    memcpy(header + 18, &w, 4);
    const int32_t bh = h; memcpy(header + 22, &bh, 4);
    const uint16_t planes = 1; memcpy(header + 26, &planes, 2);
    const uint16_t bpp = 24; memcpy(header + 28, &bpp, 2);
    memcpy(header + 34, &data_size, 4);
    fwrite(header, 1, 54, fp);
    std::vector<uint8_t> row_buf(row, 0);
    for (int y = h - 1; y >= 0; --y) {
        const uint8_t *src = rgb565 + (size_t)y * stride;
        for (int x = 0; x < w; ++x) {
            const uint16_t px = (uint16_t)(src[x * 2] | (src[x * 2 + 1] << 8));
            row_buf[x * 3 + 0] = (uint8_t)(((px >> 0) & 0x1F) << 3);       // B
            row_buf[x * 3 + 1] = (uint8_t)(((px >> 5) & 0x3F) << 2);       // G
            row_buf[x * 3 + 2] = (uint8_t)(((px >> 11) & 0x1F) << 3);      // R
        }
        fwrite(row_buf.data(), 1, row, fp);
    }
    fclose(fp);
}

void captureShot(const std::string &path, lv_display_t *disp)
{
    lv_refr_now(disp);
    lv_draw_buf_t *buf = lv_display_get_buf_active(disp);
    if (!buf || !buf->data) { fprintf(stderr, "sim: no draw buffer\n"); return; }
    const uint32_t w = buf->header.w;
    const uint32_t h = buf->header.h;
    writeBmp(path, (const uint8_t *)buf->data, (int)w, (int)h, buf->header.stride);
    printf("sim: saved %s (%ux%u)\n", path.c_str(), w, h);
}

} // namespace

int main(int argc, char **argv)
{
    const Args args = parseArgs(argc, argv);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return 1;
    }

    lv_init();
    lv_display_t *disp = lv_sdl_window_create(240, 320);
    if (!disp) { fprintf(stderr, "lv_sdl_window_create failed\n"); return 1; }
    lv_display_set_render_mode(disp, LV_DISPLAY_RENDER_MODE_FULL);
    lv_sdl_mouse_create();

    // 精灵独立测试模式：跳过 App，直接渲染指定素材目录。
    bool sprite_test = getenv("MADE_SIM_SPRITE_DIR") != nullptr;
    if (sprite_test) run_sprite_test();

    if (!sprite_test && !VibeCoding::startOnDisplay()) {
        fprintf(stderr, "sim: startOnDisplay failed\n");
        return 1;
    }

    SDL_Window *window = nullptr;
    void *renderer = lv_sdl_window_get_renderer(disp);
    if (renderer) window = SDL_RenderGetWindow((SDL_Renderer *)renderer);

    int frame = 0;
    for (const SimAction &action : args.actions) {
        if (action.type == SimAction::Tap) runTap(action, window, frame);
        else if (action.type == SimAction::Swipe) runSwipe(action, window, frame);
        else {
            for (int i = 0; i < action.frames; ++i) {
                SDL_PumpEvents(); lv_tick_inc(33); lv_timer_handler(); SDL_Delay(30); ++frame;
            }
        }
    }

    // 主循环：跑满总帧数（模拟器窗口保持打开，动画/定时器正常运转）。
    const int remaining = frame < args.total_frames ? args.total_frames - frame : 0;
    for (int i = 0; i < remaining; ++i) {
        SDL_PumpEvents();
        lv_tick_inc(33);
        lv_timer_handler();
        SDL_Delay(30);
        ++frame;
    }

    if (!args.shot_path.empty()) {
        // 手势后等待若干帧让过渡动画收尾，再截整帧。
        for (int i = 0; i < args.settle_frames; ++i) {
            SDL_PumpEvents(); lv_tick_inc(33); lv_timer_handler(); SDL_Delay(30);
        }
        captureShot(args.shot_path, disp);
    }

    // 不退出：交互模式下窗口一直开着，直到用户关窗口（SDL_QUIT → 跳出）。
    bool quit = !args.shot_path.empty();
    while (!quit) {
        SDL_PumpEvents();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) quit = true;
        }
        lv_tick_inc(33);
        lv_timer_handler();
        SDL_Delay(30);
    }
    return 0;
}
