// SPDX-License-Identifier: Apache-2.0
#include "vibe_touch_keyboard.hpp"

#include <cstring>
#include <new>

namespace vibe_touch_keyboard {
namespace {
struct State {
    lv_obj_t *target;
    unsigned page = 0;
    bool uppercase = false;
    bool numeric = false;
    bool number_first = false;
};

// Every page is three rows of five characters plus a row of five actions.
// Together the pages cover printable ASCII, including Wi-Fi password symbols.
constexpr const char *text_pages[][24] = {
    {"a", "b", "c", "d", "e", "\n", "f", "g", "h", "i", "j", "\n", "k", "l", "m", "n", "o", "\n", "Aa", "Next", "Space", "Del", "OK", ""},
    {"p", "q", "r", "s", "t", "\n", "u", "v", "w", "x", "y", "\n", "z", "0", "1", "2", "3", "\n", "Aa", "Next", "Space", "Del", "OK", ""},
    {"4", "5", "6", "7", "8", "\n", "9", ".", "-", "_", "@", "\n", "/", ":", ";", "!", "?", "\n", "Aa", "Next", "Space", "Del", "OK", ""},
    {"+", "=", "#", "$", "%", "\n", "&", "*", "(", ")", "[", "\n", "]", "{", "}", "<", ">", "\n", "Aa", "Next", "Space", "Del", "OK", ""},
    {"\\", "|", "^", "~", "`", "\n", "'", "\"", ",", "0", "1", "\n", "2", "3", "4", "5", "6", "\n", "Aa", "Next", "Space", "Del", "OK", ""},
};
constexpr unsigned page_count = sizeof(text_pages) / sizeof(text_pages[0]);
constexpr const char *number_map[] = {
    "0", "1", "2", "3", "4", "\n",
    "5", "6", "7", "8", "9", "\n",
    "Del", "OK", ""
};
constexpr const char *number_first_map[] = {
    "0", "1", "2", "3", "4", "\n",
    "5", "6", "7", "8", "9", "\n",
    "ABC", "Del", "OK", ""
};

void on_button(lv_event_t *event)
{
    auto *state = static_cast<State *>(lv_event_get_user_data(event));
    if (!state || !state->target) return;
    lv_obj_t *matrix = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const uint32_t selected = lv_buttonmatrix_get_selected_button(matrix);
    const char *key = lv_buttonmatrix_get_button_text(matrix, selected);
    if (!key || !*key) return;
    if (std::strcmp(key, "OK") == 0) {
        lv_obj_send_event(state->target, LV_EVENT_READY, nullptr);
    } else if (std::strcmp(key, "Del") == 0) {
        lv_textarea_delete_char(state->target);
    } else if (std::strcmp(key, "Space") == 0) {
        lv_textarea_add_text(state->target, " ");
    } else if (std::strcmp(key, "Next") == 0) {
        state->page = (state->page + 1) % (page_count + (state->number_first ? 1 : 0));
        lv_buttonmatrix_set_map(matrix, state->page == page_count ? number_first_map : text_pages[state->page]);
    } else if (!state->numeric && std::strcmp(key, "ABC") == 0) {
        state->page = 0;
        lv_buttonmatrix_set_map(matrix, text_pages[state->page]);
    } else if (std::strcmp(key, "Aa") == 0) {
        state->uppercase = !state->uppercase;
    } else {
        char character[2] = {key[0], 0};
        if (state->uppercase && character[0] >= 'a' && character[0] <= 'z')
            character[0] = static_cast<char>(character[0] - 'a' + 'A');
        lv_textarea_add_text(state->target, character);
    }
}

void on_delete(lv_event_t *event)
{
    delete static_cast<State *>(lv_event_get_user_data(event));
}
} // namespace

lv_obj_t *create(lv_obj_t *parent, lv_obj_t *target,
                 int x, int y, int width, int height, bool numeric, bool number_first)
{
    const bool with_number_page = number_first && !numeric;
    auto *state = new (std::nothrow) State{target, with_number_page ? page_count : 0,
                                          false, numeric, with_number_page};
    if (!state) return nullptr;
    lv_obj_t *matrix = lv_buttonmatrix_create(parent);
    lv_obj_set_align(matrix, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(matrix, x, y);
    lv_obj_set_size(matrix, width, height);
    lv_obj_set_style_bg_color(matrix, lv_color_hex(0x10243B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(matrix, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(matrix, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(matrix, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(matrix, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_column(matrix, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(matrix, lv_color_hex(0x31547A), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(matrix, lv_color_hex(0x2A977F), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(matrix, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(matrix, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_radius(matrix, 7, LV_PART_ITEMS);
    lv_buttonmatrix_set_map(matrix, numeric ? number_map : with_number_page ? number_first_map : text_pages[0]);
    lv_obj_add_event_cb(matrix, on_button, LV_EVENT_VALUE_CHANGED, state);
    lv_obj_add_event_cb(matrix, on_delete, LV_EVENT_DELETE, state);
    return matrix;
}
} // namespace vibe_touch_keyboard
