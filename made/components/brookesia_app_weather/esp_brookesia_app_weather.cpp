#include "esp_brookesia_app_weather.hpp"

#include "weather_data.hpp"

LV_IMG_DECLARE(icon_weather);

namespace esp_brookesia::apps {
namespace {
constexpr auto kText = 0xEAF4FF;
constexpr auto kMuted = 0xA6BBD0;
constexpr auto kAccent = 0x3478B8;

void styleButton(lv_obj_t* button, uint32_t background) {
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(background), 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
}

lv_obj_t* centeredLabel(lv_obj_t* parent, const char* text, int top, const lv_font_t* font,
                        uint32_t color, int width = 220) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, top);
    return label;
}
} // namespace

Weather* Weather::instance_ = nullptr;

Weather* Weather::requestInstance(bool use_status_bar, bool use_navigation_bar) {
    if (instance_ == nullptr) instance_ = new Weather(use_status_bar, use_navigation_bar);
    return instance_;
}

Weather::Weather(bool use_status_bar, bool use_navigation_bar)
    : App("Weather", &icon_weather, true, use_status_bar, use_navigation_bar) {}

bool Weather::init() {
    WeatherData::instance().start();
    return true;
}

bool Weather::run() {
    lv_obj_t* screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B172A), 0);

    lv_obj_t* panel = lv_obj_create(screen);
    lv_obj_set_size(panel, 248, 248);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, 20, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x13283E), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    centeredLabel(panel, "WEATHER", 13, &lv_font_montserrat_16, kMuted);

    lv_obj_t* city_button = lv_btn_create(panel);
    lv_obj_set_size(city_button, 174, 36);
    lv_obj_align(city_button, LV_ALIGN_TOP_MID, 0, 42);
    styleButton(city_button, 0x254968);
    city_label_ = centeredLabel(city_button, "Choose city", 8, &lv_font_montserrat_16, kText, 162);
    lv_obj_add_event_cb(city_button, openEditor, LV_EVENT_CLICKED, this);

    temperature_label_ = centeredLabel(panel, "--", 88, &lv_font_montserrat_26, kText);
    condition_label_ = centeredLabel(panel, "", 129, &lv_font_montserrat_16, kText);
    status_label_ = centeredLabel(panel, "Tap city to set location", 165,
                                  &lv_font_montserrat_12, kMuted);

    lv_obj_t* refresh_button = lv_btn_create(panel);
    lv_obj_set_size(refresh_button, 124, 31);
    lv_obj_align(refresh_button, LV_ALIGN_TOP_MID, 0, 204);
    styleButton(refresh_button, kAccent);
    centeredLabel(refresh_button, "Refresh", 7, &lv_font_montserrat_14, kText, 112);
    lv_obj_add_event_cb(refresh_button, refreshWeather, LV_EVENT_CLICKED, this);

    editor_ = lv_obj_create(screen);
    lv_obj_set_size(editor_, 248, 248);
    lv_obj_center(editor_);
    lv_obj_set_style_radius(editor_, 20, 0);
    lv_obj_set_style_bg_color(editor_, lv_color_hex(0x13283E), 0);
    lv_obj_set_style_border_width(editor_, 0, 0);
    lv_obj_set_style_pad_all(editor_, 0, 0);
    lv_obj_clear_flag(editor_, LV_OBJ_FLAG_SCROLLABLE);
    centeredLabel(editor_, "City name (English)", 9, &lv_font_montserrat_14, kText);

    city_input_ = lv_textarea_create(editor_);
    lv_obj_set_size(city_input_, 218, 36);
    lv_obj_align(city_input_, LV_ALIGN_TOP_MID, 0, 36);
    lv_textarea_set_one_line(city_input_, true);
    lv_textarea_set_max_length(city_input_, 64);
    lv_textarea_set_placeholder_text(city_input_, "Beijing, China");

    lv_obj_t* keyboard = lv_keyboard_create(editor_);
    lv_obj_set_size(keyboard, 228, 129);
    lv_obj_align(keyboard, LV_ALIGN_TOP_MID, 0, 75);
    lv_keyboard_set_textarea(keyboard, city_input_);

    lv_obj_t* cancel_button = lv_btn_create(editor_);
    lv_obj_set_size(cancel_button, 90, 32);
    lv_obj_align(cancel_button, LV_ALIGN_TOP_LEFT, 25, 208);
    styleButton(cancel_button, 0x42566B);
    centeredLabel(cancel_button, "Cancel", 8, &lv_font_montserrat_14, kText, 82);
    lv_obj_add_event_cb(cancel_button, dismissEditor, LV_EVENT_CLICKED, this);

    lv_obj_t* save_button = lv_btn_create(editor_);
    lv_obj_set_size(save_button, 90, 32);
    lv_obj_align(save_button, LV_ALIGN_TOP_RIGHT, -25, 208);
    styleButton(save_button, kAccent);
    centeredLabel(save_button, "Save", 8, &lv_font_montserrat_14, kText, 82);
    lv_obj_add_event_cb(save_button, saveCity, LV_EVENT_CLICKED, this);

    showEditor(false);
    updateView();
    timer_ = lv_timer_create(tick, 1000, this);
    return true;
}

bool Weather::back() {
    if (editor_ != nullptr && !lv_obj_has_flag(editor_, LV_OBJ_FLAG_HIDDEN)) {
        showEditor(false);
        return true;
    }
    return notifyCoreClosed();
}

bool Weather::close() {
    // Brookesia recycles the screen and timer created in run(). The worker only
    // writes its own snapshot and never touches a deleted LVGL object.
    city_label_ = nullptr;
    temperature_label_ = nullptr;
    condition_label_ = nullptr;
    status_label_ = nullptr;
    editor_ = nullptr;
    city_input_ = nullptr;
    timer_ = nullptr;
    return true;
}

void Weather::tick(lv_timer_t* timer) {
    auto* app = static_cast<Weather*>(lv_timer_get_user_data(timer));
    if (app != nullptr && app->status_label_ != nullptr) app->updateView();
}

void Weather::openEditor(lv_event_t* event) {
    auto* app = static_cast<Weather*>(lv_event_get_user_data(event));
    if (app == nullptr) return;
    const auto state = WeatherData::instance().snapshot();
    lv_textarea_set_text(app->city_input_, state.city.c_str());
    app->showEditor(true);
}

void Weather::saveCity(lv_event_t* event) {
    auto* app = static_cast<Weather*>(lv_event_get_user_data(event));
    if (app == nullptr) return;
    if (!WeatherData::instance().setCity(lv_textarea_get_text(app->city_input_))) {
        lv_obj_set_style_border_width(app->city_input_, 2, 0);
        lv_obj_set_style_border_color(app->city_input_, lv_color_hex(0xFF6969), 0);
        return;
    }
    app->showEditor(false);
    app->updateView();
}

void Weather::dismissEditor(lv_event_t* event) {
    auto* app = static_cast<Weather*>(lv_event_get_user_data(event));
    if (app != nullptr) app->showEditor(false);
}

void Weather::refreshWeather(lv_event_t* event) {
    auto* app = static_cast<Weather*>(lv_event_get_user_data(event));
    if (app != nullptr) WeatherData::instance().refresh();
}

void Weather::showEditor(bool show) {
    if (editor_ == nullptr) return;
    if (show) {
        lv_obj_set_style_border_width(city_input_, 0, 0);
        lv_obj_remove_flag(editor_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(editor_);
    } else {
        lv_obj_add_flag(editor_, LV_OBJ_FLAG_HIDDEN);
    }
}

void Weather::updateView() {
    const auto state = WeatherData::instance().snapshot();
    lv_label_set_text(city_label_, state.city.empty() ? "Choose city" : state.city.c_str());
    if (state.available) {
        lv_label_set_text_fmt(temperature_label_, "%.1f C", state.temperature_c);
        lv_label_set_text(condition_label_, state.condition.c_str());
        if (state.stale) {
            lv_label_set_text(status_label_, "Saved weather / offline");
        } else {
            lv_label_set_text(status_label_, state.observed_at.c_str());
        }
    } else {
        lv_label_set_text(temperature_label_, "--");
        lv_label_set_text(condition_label_, "");
        lv_label_set_text(status_label_, state.city.empty() ? "Tap city to set location" :
                          (state.error.empty() ? "Loading weather..." : state.error.c_str()));
    }
}

} // namespace esp_brookesia::apps
