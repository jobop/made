// SPDX-License-Identifier: Apache-2.0
// 附件模块（回复区富内容）：图片 / 链接二维码行渲染、后台抓取+解码、
// 点击放大浮层。生命周期：解码位图由 decoded_dscs_ 统一托管，浮层打开
// 期间绝不重建（浮层图像正引用 dsc）。
#include "esp_brookesia_app_vibe.hpp"
#include "vibe_ui_shared.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "made_layout.hpp"
#include "made_sprite.hpp"

namespace esp_brookesia::apps {

namespace {

// 图片附件：后台拉取 + 解码，完成后切回 LVGL 线程设置到目标 lv_image。
struct ImageFetchArg {
    std::string url;
    lv_obj_t *img;
    VibeCoding *self;
    size_t index; // 对应 current_attachments_ 中的序号，用于回写 attachment_dscs_
    std::vector<uint8_t> buf;
};

} // namespace

// 释放上一轮解码出来的图片位图，避免泄漏与悬垂。
void VibeCoding::freeDecodedImages() {
    for (auto *dsc : decoded_dscs_) {
        if (dsc) {
            heap_caps_free(const_cast<uint8_t *>(dsc->data));
            delete dsc;
        }
    }
    decoded_dscs_.clear();
    attachment_dscs_.clear();
}

void VibeCoding::renderAttachments(const std::vector<Attachment> &atts) {
    // 指纹：仅当附件集合（类型+URL+文案）变化时重建，避免每次 render 重拉图/闪屏。
    std::string sig;
    for (const auto &a : atts) sig += a.type + '\x1f' + a.url + '\x1f' + a.text + '\x1e';
    // 放大浮层打开期间：绝不重建、绝不释放 dsc（浮层图像正引用它），直接保留现状。
    if (zoom_overlay_) return;
    if (sig == attachment_sig_ && attachments_box_) return; // 无变化，维持已渲染内容
    // 集合已变（或首次）：先清旧内容再按新集合重建。
    for (auto *o : attachment_objs_) lv_obj_del(o);
    attachment_objs_.clear();
    freeDecodedImages();
    attachment_sig_ = sig;
    current_attachments_ = atts;
    attachment_dscs_.assign(atts.size(), nullptr);
    if (!answer_area_ || atts.empty()) {
        if (attachments_box_) lv_obj_add_flag(attachments_box_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_update_layout(answer_area_);
    const int label_h = lv_obj_get_height(result_label_);
    const int box_w = lv_obj_get_width(answer_area_) - 12;
    if (!attachments_box_) {
        attachments_box_ = lv_obj_create(answer_area_);
        lv_obj_set_style_bg_opa(attachments_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(attachments_box_, 0, 0);
        lv_obj_set_style_pad_all(attachments_box_, 0, 0);
        lv_obj_set_style_pad_row(attachments_box_, 8, 0);
        lv_obj_set_flex_flow(attachments_box_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(attachments_box_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_remove_flag(attachments_box_, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_set_pos(attachments_box_, 6, label_h + 8);
    lv_obj_set_size(attachments_box_, box_w, LV_SIZE_CONTENT);
    lv_obj_clear_flag(attachments_box_, LV_OBJ_FLAG_HIDDEN);

    for (size_t i = 0; i < atts.size(); ++i) {
        const Attachment &a = atts[i];
        lv_obj_t *row = lv_obj_create(attachments_box_);
        lv_obj_set_size(row, box_w, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(VT(btn_normal)), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_all(row, 5, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_user_data(row, reinterpret_cast<void *>(i));  // 用索引回查 current_attachments_
        lv_obj_add_event_cb(row, attachmentTapCallback, LV_EVENT_CLICKED, this);
        attachment_objs_.push_back(row);

        if (a.type == "image") {
            lv_obj_t *img = lv_image_create(row);
            lv_obj_set_size(img, 84, 84);
            lv_obj_t *cap = lv_label_create(row);
            lv_label_set_text(cap, "图片加载中…");
            lv_obj_set_width(cap, box_w - 100);
            lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_color(cap, lv_color_hex(VT(text_hint)), 0);
            startImageFetch(a.url, img, i);
        } else {
            lv_obj_t *qr = lv_qrcode_create(row);
            lv_qrcode_set_size(qr, 84);
            lv_qrcode_set_dark_color(qr, lv_color_black());
            lv_qrcode_set_light_color(qr, lv_color_white());
            lv_qrcode_set_quiet_zone(qr, true);
            lv_qrcode_update(qr, a.url.c_str(), a.url.size());
            lv_obj_t *cap = lv_label_create(row);
            lv_label_set_text(cap, a.text.empty() ? a.url.c_str() : a.text.c_str());
            lv_obj_set_width(cap, box_w - 100);
            lv_label_set_long_mode(cap, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_color(cap, lv_color_hex(VT(text_hint)), 0);
        }
    }
    lv_obj_update_layout(answer_area_);
}

void VibeCoding::attachmentTapCallback(lv_event_t *event) {
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    lv_obj_t *row = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const size_t idx = reinterpret_cast<size_t>(lv_obj_get_user_data(row));
    if (idx < self->current_attachments_.size()) self->openZoom(idx);
}

void VibeCoding::zoomCloseCallback(lv_event_t *event) {
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self) return;
    if (self->zoom_overlay_) {
        lv_obj_del(self->zoom_overlay_);
        self->zoom_overlay_ = nullptr;
    }
}

void VibeCoding::openZoom(size_t idx) {
    if (idx >= current_attachments_.size()) return;
    const Attachment &a = current_attachments_[idx];
    if (zoom_overlay_) lv_obj_del(zoom_overlay_);
    zoom_overlay_ = lv_obj_create(lv_scr_act());
    lv_obj_set_size(zoom_overlay_, made_screen_w(), made_screen_h());
    lv_obj_set_style_bg_color(zoom_overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(zoom_overlay_, LV_OPA_90, 0);
    lv_obj_set_style_border_width(zoom_overlay_, 0, 0);
    lv_obj_set_style_pad_all(zoom_overlay_, 0, 0);
    lv_obj_remove_flag(zoom_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(zoom_overlay_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(zoom_overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zoom_overlay_, zoomCloseCallback, LV_EVENT_CLICKED, this);

    lv_obj_t *content = nullptr;
    if (a.type == "image") {
        lv_obj_t *img = lv_image_create(zoom_overlay_);
        // 按索引取本附件对应的已解码图（dsc 由 freeDecodedImages 托管生命周期；
        // 浮层打开期间不会被清理，因为 renderAttachments 在重建前会先关掉旧浮层）。
        lv_image_dsc_t *dsc = (idx < attachment_dscs_.size()) ? attachment_dscs_[idx] : nullptr;
        if (dsc) {
            lv_image_set_src(img, dsc);
            lv_obj_set_size(img, std::min<int>(made_screen_w() - 20, static_cast<int>(dsc->header.w)),
                                  std::min<int>(made_screen_h() - 60, static_cast<int>(dsc->header.h)));
            content = img;
        } else {
            lv_label_set_text(lv_label_create(zoom_overlay_),
                ("图片加载中或不可用\n" + a.url).c_str());
        }
    } else {
        lv_obj_t *qr = lv_qrcode_create(zoom_overlay_);
        lv_qrcode_set_size(qr, std::min(made_screen_w() - 24, 280));
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_set_quiet_zone(qr, true);
        lv_qrcode_update(qr, a.url.c_str(), a.url.size());
        content = qr;
    }
    if (content) lv_obj_center(content);
    lv_obj_t *hint = lv_label_create(zoom_overlay_);
    lv_label_set_text(hint, "点击任意处关闭");
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);
}

// lv_async_call 的回调必须是自由函数（非成员），签名为 void(*)(void*)。
// 解码由本函数完成，建 dsc / 写私有成员交给成员方法 onImageFetched（可访问私有数据）。
static void imageFetchDoneCb(void *arg_void) {
    auto *arg = static_cast<ImageFetchArg *>(arg_void);
    if (!arg || !arg->self) { delete arg; return; }
    if (arg->img && lv_obj_is_valid(arg->img) && !arg->buf.empty()) {
        int w = 0, h = 0;
        uint8_t *px = made_sprite::decodePngArgbMem(arg->buf.data(), arg->buf.size(), w, h);
        if (px) {
            // 缩略图只有 84px，放大也不超过屏幕。按最长边收到 360，避免一张照片按原尺寸占满 PSRAM。
            const int cap = std::max(84, std::min(made_screen_w(), 360));
            if (w > cap || h > cap) {
                const int dw = w >= h ? cap : std::max(1, w * cap / h);
                const int dh = h >= w ? cap : std::max(1, h * cap / w);
                auto *dst = static_cast<uint8_t *>(heap_caps_malloc(
                    static_cast<size_t>(dw) * static_cast<size_t>(dh) * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (dst) {
                    for (int y = 0; y < dh; ++y) {
                        const int sy = y * h / dh;
                        for (int x = 0; x < dw; ++x) {
                            const int sx = x * w / dw;
                            memcpy(dst + (static_cast<size_t>(y) * dw + x) * 4,
                                   px + (static_cast<size_t>(sy) * w + sx) * 4, 4);
                        }
                    }
                    heap_caps_free(px);
                    px = dst;
                    w = dw;
                    h = dh;
                }
            }
            arg->self->onImageFetched(arg->index, px, w, h, arg->img);
        }
    }
    delete arg;
}

void VibeCoding::onImageFetched(size_t index, uint8_t *px, int w, int h, lv_obj_t *img) {
    auto *dsc = new lv_image_dsc_t();
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc->header.w = static_cast<uint32_t>(w);
    dsc->header.h = static_cast<uint32_t>(h);
    dsc->header.stride = static_cast<uint32_t>(w) * 4;
    dsc->data_size = static_cast<uint32_t>(w) * h * 4;
    dsc->data = px;
    decoded_dscs_.push_back(dsc);
    // 回写按索引映射：异步回调可能晚于下一次 render，需防越界。
    if (index < attachment_dscs_.size()) attachment_dscs_[index] = dsc;
    if (img && lv_obj_is_valid(img)) {
        lv_image_set_src(img, dsc);
        lv_obj_set_size(img, 84, 84);
    }
}

void VibeCoding::startImageFetch(const std::string &url, lv_obj_t *img, size_t index) {
    auto *arg = new ImageFetchArg{url, img, this, index, {}};
    if (xTaskCreate([](void *p) {
            auto *a = static_cast<ImageFetchArg *>(p);
            a->buf = a->self->fetchUrlBytes(a->url, 512 * 1024);
            lv_async_call(imageFetchDoneCb, a);
            vTaskDelete(nullptr);
        }, "img_fetch", 8192, arg, 2, nullptr) != pdPASS) {
        delete arg;
    }
}

// 直接拉取任意 http(s) URL 的字节（图片附件用）。返回空表示失败。
std::vector<uint8_t> VibeCoding::fetchUrlBytes(const std::string &url, size_t max) {
    std::vector<uint8_t> out;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 8000;
    cfg.buffer_size = 4096;
    auto *client = esp_http_client_init(&cfg);
    if (!client) return out;
    if (esp_http_client_open(client, 0) != ESP_OK) { esp_http_client_cleanup(client); return out; }
    const int len = esp_http_client_fetch_headers(client);
    if (len < 0) { esp_http_client_cleanup(client); return out; }
    int total = 0;
    char buf[1024];
    int r;
    while ((r = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
        if (static_cast<size_t>(total + r) > max) { esp_http_client_cleanup(client); return out; }
        out.insert(out.end(), buf, buf + r);
        total += r;
    }
    esp_http_client_cleanup(client);
    return out;
}

} // namespace esp_brookesia::apps
