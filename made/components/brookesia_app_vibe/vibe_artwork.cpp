#include "esp_brookesia_app_vibe.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>

#include "board_voice_audio.h"
#include "boot_click_gesture.hpp"
#include "made_layout.hpp"
#include "made_sprite.hpp"
#include "made_sprite_cache.hpp"
#include "new_message_chime.hpp"
#include "vibe_motion.hpp"
#include "cJSON.h"
#include "bsp/display.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "mbedtls/md.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "vibe_pairing.hpp"
#include "vibe_voice_capture.hpp"
#include "vibe_wifi.hpp"
#include "vibe_theme.hpp"
#include "vibe_usb.hpp"
#include "vibe_i18n.hpp"
#include "vibe_touch_keyboard.hpp"
#include "vibe_phone_setup.hpp"

using esp_brookesia::systems::phone::App;

LV_IMG_DECLARE(img_app_vibe);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_14_4);
#include "vibe_ui_shared.hpp"

namespace esp_brookesia::apps {

void VibeCoding::invalidateCatalog(bool clear)
{
    std::lock_guard<std::mutex> lock(model_mutex_);
    catalog_epoch_.fetch_add(1);
    config_loaded_.store(false);
    voice_available_.store(false);
    visible_task_id_.clear();
    if (clear) {
        providers_.clear();
        selected_provider_.store(0);
        catalog_identity_.clear();
        model_.sessions.clear();
        model_.tasks.clear();
        model_.feedback.clear();
        model_.feedback_task_id.clear();
        model_.session_notice.clear();
        voice_session_id_.clear();
        pending_action_ = {};
        model_.action_in_flight = false;
        pending_create_session_ = false;
        pending_create_provider_id_.clear();
        pending_create_project_id_.clear();
    }
    board_events_.clear();
    pending_board_event_.clear();
    board_catalog_sent_ = false;
    ++model_.revision;
}

// 形象素材下载（worker 线程）。目标与最近一次尝试一致且成功过就不再动网络。
// 曾经在待确认任务在屏时整体跳过素材下载（怕分片拖慢确认链路），但 USB
// 是请求/响应串行协议，分片不会吞掉命令应答，最多慢一两拍；而待确认可能
// 无限期驻留（用户不点 BOOT），门禁会把素材永久卡死（2026-10-18 实锤：
// 假 OTA 任务常驻 waiting_confirmation 期间素材一直加载不出）——已删除。
void VibeCoding::syncArtwork()
{
    std::string provider;
    vibe_provider::SpriteInfo sprite;
    size_t provider_count = 0;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        const auto &selected = selectedProviderLocked();
        provider = selected.id;
        sprite = selected.sprite;
        provider_count = providers_.size();
    }
    {
        std::lock_guard<std::mutex> lock(artwork_mutex_);
        if (artwork_attempted_ && artwork_target_ == provider &&
            artwork_target_version_ == (sprite.valid() ? sprite.version : 0) &&
            !artworkRetryDue()) {
            ESP_LOGI(BUTTON_TAG, "[sprite] skip=already_attempted provider=%s version=%u",
                provider.c_str(), static_cast<unsigned>(artwork_target_version_));
            return;
        }
        if (artwork_target_ != provider ||
            artwork_target_version_ != (sprite.valid() ? sprite.version : 0)) {
            artwork_retry_count_ = 0;  // 换了目标/版本，重试预算重新计
        }
        artwork_target_ = provider;
        artwork_target_version_ = sprite.valid() ? sprite.version : 0;
        artwork_attempted_ = true;
    }
    if (!sprite.valid()) {
        ESP_LOGI(BUTTON_TAG, "[sprite] skip=no_sprite provider=%s providers=%u",
            provider.c_str(), static_cast<unsigned>(provider_count));
        std::lock_guard<std::mutex> lock(artwork_mutex_);
        artwork_ready_provider_.clear();
        artwork_dirty_.store(true);
        return;
    }
    ESP_LOGI(BUTTON_TAG, "[sprite] fetch provider=%s version=%u files=%u",
        provider.c_str(), static_cast<unsigned>(sprite.version), static_cast<unsigned>(sprite.files.size()));
    const auto fetch = [this](const std::string &path, std::string &out, int &status) {
        return request(path, false, out, status, "", 200 * 1024, 30000);
    };
    bool manifest_changed = false;
    const bool ok = made_sprite_cache::ensure(provider, sprite, fetch, &manifest_changed);
    ESP_LOGI(BUTTON_TAG, "[sprite] provider=%s version=%u ok=%d%s", provider.c_str(),
        static_cast<unsigned>(sprite.version), ok ? 1 : 0,
        manifest_changed ? " manifest_refreshed" : "");
    std::lock_guard<std::mutex> lock(artwork_mutex_);
    if (ok && manifest_changed && artwork_applied_ == provider) {
        // 缓存 manifest 被修正（此前可能截断残缺、动作/命令丢失）：清掉
        // 「已应用」记忆与直换 memo，强制 LVGL 侧用修好的 manifest 重挂素材，
        // 动作名单（deviceActions）才能正确上报。
        artwork_applied_.clear();
        artwork_applied_version_ = 0;
        cached_apply_provider_.clear();
        cached_apply_version_ = 0;
        cached_apply_attempts_ = 0;
        artwork_request_.store(true);
    }
    if (ok) {
        artwork_retry_at_ms_ = 0;
        artwork_retry_count_ = 0;
    } else if (++artwork_retry_count_ <= 5) {
        // 失败 15s 后重试，最多 5 次；配置或选中助手变化会重置预算。
        artwork_retry_at_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000) + 15000;
        ESP_LOGI(BUTTON_TAG, "[sprite] retry_scheduled provider=%s attempt=%u",
            provider.c_str(), static_cast<unsigned>(artwork_retry_count_));
    } else {
        artwork_retry_at_ms_ = 0;  // 预算用尽：等下一次 provider/版本变化再试
        ESP_LOGI(BUTTON_TAG, "[sprite] retry_exhausted provider=%s", provider.c_str());
    }
    artwork_ready_provider_ = ok ? provider : std::string();
    artwork_dirty_.store(true);
}

bool VibeCoding::artworkRetryDue() const
{
    const uint32_t at = artwork_retry_at_ms_;
    return at != 0 && static_cast<uint32_t>(esp_timer_get_time() / 1000) >= at;
}

// 开机预加载（worker 线程）：目录就绪后把所有带素材的助手排队，后台逐个
// 把素材缓存拉齐——滑动切换走 tryApplyCachedArtwork 只看本地 manifest，
// 缓存齐了就是秒换，不用现等下载。约束：每轮循环只处理一个（下载最坏
// 30s，不能把 worker 循环堵住 delaying 命令下发）；已缓存（manifest 版本
// 一致）直接跳过，不做哈希复核。曾经待确认在屏时整体放弃（怕抢链路），
// 理由同 syncArtwork：USB 串行协议分片不吞命令应答，而待确认可能无限期
// 驻留，门禁只会把预载永久卡死——已删除（2026-10-18）。
void VibeCoding::preloadArtworkCache()
{
    const uint64_t epoch = catalog_epoch_.load();
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (!config_loaded_.load() || providers_.empty()) return;
        if (artwork_preload_epoch_ != epoch) {
            artwork_preload_epoch_ = epoch;
            artwork_preload_queue_.clear();
            for (const auto &entry : providers_)
                if (entry.sprite.valid())
                    artwork_preload_queue_.push_back({entry.id, entry.sprite.version, 0});
        }
        if (artwork_preload_queue_.empty()) return;
    }
    for (;;) {
        std::string provider;
        uint32_t version = 0;
        uint8_t attempts = 0;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            if (artwork_preload_queue_.empty()) return;
            provider = artwork_preload_queue_.front().provider;
            version = artwork_preload_queue_.front().version;
            attempts = artwork_preload_queue_.front().attempts;
        }
        // 目录刷新后助手可能消失或版本已变：消失/失效的丢掉；版本变了说明
        // 桥接器侧发了新素材——用新版本重新入队（否则快照过期条目被丢弃后，
        // 要等到下次桥接器重连才会再试，期间新素材永远下载不到）。
        vibe_provider::SpriteInfo sprite;
        bool valid = false;
        bool requeue = false;
        uint32_t fresh_version = 0;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            const auto *entry = findProviderLocked(provider);
            valid = entry && entry->sprite.valid() && entry->sprite.version == version;
            requeue = !valid && entry && entry->sprite.valid();
            if (valid) sprite = entry->sprite;
            if (requeue) fresh_version = entry->sprite.version;
        }
        bool done = true;
        if (!valid) {
            if (requeue) {
                std::lock_guard<std::mutex> lock(model_mutex_);
                for (auto it = artwork_preload_queue_.begin();
                     it != artwork_preload_queue_.end(); ++it) {
                    if (it->provider == provider && it->version == version) {
                        it->version = fresh_version;
                        it->attempts = 0;
                        break;
                    }
                }
            }
            // 丢掉（或已换新版本），继续看下一个（本轮内循环，队列很快清空）。
        } else {
            uint32_t cached = 0;
            if (made_sprite_cache::cachedVersion(provider, cached) && cached == version) {
                // 已就绪，无需网络。若正是当前选中助手，syncArtwork 在管它。
            } else if (attempts >= 2) {
                ESP_LOGW(BUTTON_TAG, "[sprite] preload_exhausted provider=%s", provider.c_str());
            } else {
                ESP_LOGI(BUTTON_TAG, "[sprite] preload provider=%s version=%u attempt=%u",
                    provider.c_str(), static_cast<unsigned>(version),
                    static_cast<unsigned>(attempts + 1));
                const auto fetch = [this](const std::string &path, std::string &out, int &status) {
                    return request(path, false, out, status, "", 200 * 1024, 30000);
                };
                done = made_sprite_cache::ensure(provider, sprite, fetch);
                if (!done) {
                    // 失败挪到队尾重试（同版本预算 2 次），本轮到此为止。
                    std::lock_guard<std::mutex> lock(model_mutex_);
                    for (auto it = artwork_preload_queue_.begin();
                         it != artwork_preload_queue_.end(); ++it) {
                        if (it->provider == provider && it->version == version) {
                            ArtworkPreloadEntry retry{*it};
                            retry.attempts = static_cast<uint8_t>(attempts + 1);
                            artwork_preload_queue_.erase(it);
                            artwork_preload_queue_.push_back(std::move(retry));
                            break;
                        }
                    }
                    return;
                }
            }
        }
        std::lock_guard<std::mutex> lock(model_mutex_);
        for (auto it = artwork_preload_queue_.begin(); it != artwork_preload_queue_.end(); ++it) {
            if (it->provider == provider && it->version == version) {
                artwork_preload_queue_.erase(it);
                break;
            }
        }
        return;  // 一轮只处理一个助手，保持 worker 循环响应
    }
}

// 侧台应挂的助手 id 与版本（当前选中助手的相邻邻居；单助手、邻居无素材
// 时 target 为空，表示这一侧不该挂任何东西）。
bool VibeCoding::sideTarget(int index, std::string &target, uint32_t &version)
{
    target.clear();
    version = 0;
    if (index < 0 || index > 1) return false;
    const SideSlot &slot = sides_[index];
    if (!slot.stage) return false;
    std::lock_guard<std::mutex> lock(model_mutex_);
    if (!config_loaded_.load() || providers_.empty()) return false;
    const int count = static_cast<int>(providers_.size());
    const int selected = selected_provider_.load();
    const int neighbor = (selected + slot.side + count) % count;
    const auto &entry = providers_[static_cast<size_t>(neighbor)];
    if (count > 1 && neighbor != selected && entry.sprite.valid()) {
        target = entry.id;
        version = entry.sprite.version;
    }
    return true;
}

// LVGL 线程。确保 sides_[index] 挂着当前选中助手的相邻助手（版本一致）。
// 内容已对（三舞台轮换的常态）就什么都不做——这就是「任何方向随时可翻」；
// 不对且解码空闲就起飞后台解码补齐（单飞，1s 失败退避）。两个侧台都缺
// 内容时每帧按 0→1 顺序排队补，拖动方向由 maybeStartPrefetch 优先插队。
void VibeCoding::ensureSide(int index)
{
    if (!sprite_stage_ || index < 0 || index > 1) return;
    SideSlot &slot = sides_[index];
    if (!slot.stage) return;
    std::string target;
    uint32_t version = 0;
    sideTarget(index, target, version);
    // 临时诊断：侧台不预载时这条日志直接指认是哪个静默分支拦的（5s 限频）。
    {
        static uint32_t diag_ms[2] = {0, 0};
        const uint32_t now_diag = lv_tick_get();
        if (now_diag - diag_ms[index] > 5000) {
            diag_ms[index] = now_diag;
            ESP_LOGI(BUTTON_TAG,
                "[sprite][diag] side%+d stage=%d main=%d target='%s' ver=%u have='%s/%u' "
                "defer_ms=%u drag=%d pf(prov='%s' inflight=%d ready=%d) "
                "free=%uKB largest=%uKB wm=%uKB",
                slot.side, slot.stage ? 1 : 0, sprite_stage_ ? 1 : 0,
                target.c_str(), (unsigned)version, slot.provider.c_str(),
                (unsigned)slot.version,
                (unsigned)(slot.defer_until > now_diag ? slot.defer_until - now_diag : 0),
                drag_side_, prefetch_provider_.c_str(),
                made_sprite::prefetchInFlight() ? 1 : 0,
                made_sprite::prefetchReady() ? 1 : 0,
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024));
        }
    }
    // —— 通用水位阀（3s 限频，随 index==0 每对侧台只跑一次）——
    // 「heap 越用越少就是有问题」：可支配内存（预取备胎、两侧台解码像素）
    // 在水位跌破安全线时主动让位，需要时由 ensureSide/maybeStartPrefetch
    // 自动补齐——边用边回收，保持稳定水位。主舞台（正显示的助手）永不
    // 释放。安全线 768KB：录音兜底线 1150KB 之前就该动手，留出反应窗口。
    if (index == 0) {
        static uint32_t valve_ms = 0;
        const uint32_t now_valve = lv_tick_get();
        if (now_valve - valve_ms > 3000) {
            valve_ms = now_valve;
            const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            if (free_now < 768 * 1024) {
                const bool drop_prefetch =
                    made_sprite::prefetchInFlight() || made_sprite::prefetchReady() ||
                    !prefetch_provider_.empty();
                made_sprite::prefetchCancel();
                if (drop_prefetch) {
                    prefetch_provider_.clear();
                    prefetch_version_ = 0;
                    prefetch_slot_ = -1;
                }
                // LVGL 图片缓存（附件放大图等的解码产物）同属可支配内存：
                // 压力下整池丢弃，需要时重新解码。放在撤侧台之前——它最便宜。
                lv_image_cache_drop(nullptr);
                lv_image_header_cache_drop(nullptr);
                for (SideSlot &s : sides_) {
                    if (s.stage && !s.provider.empty()) {
                        made_sprite::clearArtwork(s.stage);
                        s.provider.clear();
                        s.version = 0;
                        s.defer_until = now_valve + 8000;  // 防振荡：让位后别立刻抢回
                    }
                }
                const size_t free_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                ESP_LOGW(BUTTON_TAG,
                    "[heap] pressure_release free=%uKB -> %uKB "
                    "wm=%uKB largest=%uKB",
                    (unsigned)(free_now / 1024), (unsigned)(free_after / 1024),
                    (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024),
                    (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
            }
        }
    }
    // 常驻的就三套（左中右）。邻居没有素材、或只有一个助手（左右邻居
    // 都是 自己，不值得解码第二份）时，这一侧不该挂着任何东西——
    // 超出范围的旧素材就地回收，PSRAM 让出去。
    if (target.empty()) {
        if (!slot.provider.empty()) {
            made_sprite::clearArtwork(slot.stage);
            slot.provider.clear();
            slot.version = 0;
            ESP_LOGI(BUTTON_TAG, "[sprite] neighbor_recycled slot=%+d", slot.side);
        }
        return;
    }
    // 过期内容必须立刻撤下：>3 个助手时每次切换后远侧台都挂着上一次的
    // 邻居（轮换无法自洽，靠后台解码补齐）。补齐需要一两秒，期间若用户
    // 拖动，滑入视野的会是错误形象——用户会把上一个助手的邻居当成目标
    // 助手。宁可滑入空白（解码完自动补上），也不能滑入错的。
    if (!slot.provider.empty() && slot.provider != target) {
        made_sprite::clearArtwork(slot.stage);
        ESP_LOGI(BUTTON_TAG, "[sprite] neighbor_stale_cleared slot=%+d had=%s want=%s",
            slot.side, slot.provider.c_str(), target.c_str());
        slot.provider.clear();
        slot.version = 0;
    }
    if (slot.provider == target && slot.version == version) return;   // 已就绪（常态）
    // 被回收让位的侧台暂缓补齐：它刚把空间让给方向侧，立刻重试只会把
    // 空间又抢回去（掐掉方向侧的在飞解码），来回振荡。暂缓到期后自然
    // 恢复；拖动方向转到这一侧时由 maybeStartPrefetch 立即解除暂缓。
    if (lv_tick_get() < slot.defer_until && index != drag_side_) return;
    if (!prefetch_provider_.empty() || made_sprite::prefetchInFlight() ||
        made_sprite::prefetchReady())
        return;  // 解码在飞/待提交：本帧等它，下帧轮到这里再排
    // PSRAM 预算回收：助手 >3 个时第 4 套解码像素常驻放不下（实测 4 套
    // 并存后 free 常 <1.5MB，prefetchBegin 拒绝 → 每秒退避空转 → 同向
    // 第三屏永不预载）。方向侧起飞前余量不足就回收背向侧台（用户背对
    // 它，暂时用不到），腾出空间；被回收侧暂缓补齐防振荡。只允许方向
    // 侧回收背向侧——反向宁可等（背向侧不急，1s 退避无害），否则方向
    // 侧会被背向侧的补齐挤掉，来回拆台。
    const size_t kDecodeBudget = 1560 * 1024;  // 略高于 prefetchBegin 的 1.5MB guard
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < kDecodeBudget &&
        index == drag_side_) {
        SideSlot &other = sides_[1 - index];
        if (other.stage && !other.provider.empty() && other.provider != target) {
            made_sprite::clearArtwork(other.stage);
            ESP_LOGI(BUTTON_TAG,
                "[sprite] neighbor_recycled_for_budget slot=%+d had=%s for_slot=%+d want=%s",
                other.side, other.provider.c_str(), slot.side, target.c_str());
            other.provider.clear();
            other.version = 0;
            other.defer_until = lv_tick_get() + 5000;
        }
    }
    // 失败退避：起飞失败/解码失败后 1s 内不白试；重试前必须清 failed/done
    // 残留——prefetchBegin 见这些状态会直接拒绝，不清就是永久卡死。
    const uint32_t now = lv_tick_get();
    if (now < prefetch_retry_ms_) return;
    made_sprite::prefetchCancel();
    std::string manifest;
    if (made_sprite_cache::loadManifest(target, manifest) &&
        made_sprite::prefetchBegin(manifest, made_sprite_cache::baseDir(target))) {
        prefetch_provider_ = target;
        prefetch_version_ = version;
        prefetch_slot_ = index;
        ESP_LOGI(BUTTON_TAG, "[sprite] prefetch_start slot=%+d provider=%s version=%u",
            slot.side, target.c_str(), static_cast<unsigned>(version));
    } else {
        prefetch_retry_ms_ = now + 1000;
    }
}

// 录音前兜底（LVGL 线程，vibe_voice::start 之前调用）。录音缓冲一次要
// ~940KB PSRAM，而空闲常在 0.8~2.1MB 间波动——贴着悬崖边抽签，波动到
// 谷底那一刻按下说话就报「录音内存不足」。空间不足时把**非显示中**的
// 素材资源临时让位：两侧台各一套解码像素 + 在飞/就绪的预取暂存。
// 主舞台（正显示的助手）不动。录音期间手势全部禁用，不存在边录边补齐
// 的竞争；defer 30s 覆盖最长录音，结束后 prefetchTick 解除暂缓自动补齐。
void VibeCoding::reclaimSpriteMemoryForVoice()
{
    const size_t kVoiceNeed = 1150 * 1024;  // 录音缓冲 ~940KB + 余量
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram >= kVoiceNeed) return;

    const uint32_t now = lv_tick_get();
    if (made_sprite::prefetchInFlight() || made_sprite::prefetchReady() ||
        !prefetch_provider_.empty()) {
        made_sprite::prefetchCancel();
        prefetch_provider_.clear();
        prefetch_version_ = 0;
        prefetch_slot_ = -1;
        ESP_LOGI(BUTTON_TAG, "[voice] prefetch dropped for recording (free=%uKB)",
                 (unsigned)(free_psram / 1024));
    }
    for (SideSlot &slot : sides_) {
        if (slot.stage && !slot.provider.empty()) {
            made_sprite::clearArtwork(slot.stage);
            slot.provider.clear();
            slot.version = 0;
            slot.defer_until = now + 30000;  // 录音最长 30s，期间别来抢内存
        }
    }
    drag_side_ = -1;  // 让 ensureSide 的 defer 条件对两侧同时生效
    sprite_held_for_voice_ = true;
    // 附件大图和侧台抢的是同一块 PSRAM。录音时放大浮层不该还占着解码缓冲。
    if (!zoom_overlay_) freeDecodedImages();
    ESP_LOGW(BUTTON_TAG,
             "[voice] sprite reclaim for recording: free=%uKB -> %uKB",
             (unsigned)(free_psram / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

// LVGL 线程。拖动方向已明确：优先补齐该方向的侧台。三舞台轮换的常态下
// 侧台内容早已就绪，这里什么都不用做（零解码零等待）。
void VibeCoding::maybeStartPrefetch(int direction)
{
    if (direction == 0) return;
    const int index = direction < 0 ? 0 : 1;
    drag_side_ = index;
    // 方向转到这一侧：解除它的回收暂缓——用户正盯着这看，若空间仍紧，
    // ensureSide 会照规则回收对侧（此刻的背向侧）腾地方。
    sides_[index].defer_until = 0;
    // 拖动方向侧内容不对、而单飞解码在飞的是另一侧时：掐掉它改飞本侧。
    // 用户正盯着这个方向看，另一侧的解码晚点重跑没有任何代价；反过来
    // 让方向侧等一整套解码，过期/空白素材就会跟着手滑进视野。
    SideSlot &slot = sides_[index];
    std::string target;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (!config_loaded_.load() || providers_.empty()) return;
        const int count = static_cast<int>(providers_.size());
        const int selected = selected_provider_.load();
        const int neighbor = (selected + slot.side + count) % count;
        if (count > 1 && neighbor != selected &&
            providers_[static_cast<size_t>(neighbor)].sprite.valid())
            target = providers_[static_cast<size_t>(neighbor)].id;
    }
    const bool wrong = slot.provider != target ||
        (prefetch_slot_ == index && prefetch_provider_ != target);
    if (!wrong) { ensureSide(index); return; }
    if (made_sprite::prefetchInFlight() && prefetch_slot_ != index) {
        made_sprite::prefetchCancel();
        prefetch_provider_.clear();
        prefetch_version_ = 0;
        prefetch_slot_ = -1;
    }
    ensureSide(index);
}

// 每帧（maybeApplyArtwork）：在飞解码完成后挂上归属侧台，然后把两侧台
// 内容对齐当前选中助手的左右邻居（轮换后的远侧台、目录版本刷新都会在这
// 里自动补齐）。素材随即随拖动滑入视野——真翻页，不用等翻页提交。
void VibeCoding::prefetchTick()
{
    // 空闲 PSRAM 掉到 512KB 以下时，先丢掉两侧未显示的解码帧，主舞台不动。
    // （阈值对齐帧槽池的 kPoolReserveBytes：三套全驻留的均衡点约 600KB，
    //  阀太高会让侧台在均衡点附近反复清了又补、永不驻留。）
    if (!home_dragging_ && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 512 * 1024) {
        const uint32_t tight_now = lv_tick_get();
        for (SideSlot &slot : sides_) {
            if (!slot.stage || slot.provider.empty()) continue;
            made_sprite::clearArtwork(slot.stage);
            slot.provider.clear();
            slot.version = 0;
            slot.defer_until = tight_now + 8000;
        }
    }
    // 录音让位回收过的侧台：录音（含上传）一结束就解除暂缓，别让用户
    // 盯着空白侧台等满 30s。start 失败时同样在这里下一帧就恢复。
    if (sprite_held_for_voice_) {
        const auto vp = vibe_voice::status().phase;
        if (vp != vibe_voice::Phase::Recording && vp != vibe_voice::Phase::Uploading) {
            sprite_held_for_voice_ = false;
            for (SideSlot &slot : sides_) slot.defer_until = 0;
            ESP_LOGI(BUTTON_TAG, "[voice] sprite defer released after voice end");
        }
    }
    if (!prefetch_provider_.empty()) {
        if (made_sprite::prefetchReady()) {
            SideSlot &slot = sides_[prefetch_slot_ >= 0 ? prefetch_slot_ : 0];
            bool stale = false;
            {
                std::lock_guard<std::mutex> lock(model_mutex_);
                const auto *entry = findProviderLocked(prefetch_provider_);
                stale = !entry || !entry->sprite.valid() ||
                    entry->sprite.version != prefetch_version_;
            }
            if (stale) {
                // 目录刷新后素材版本变了：暂存帧过期，作废等重登记。
                made_sprite::prefetchCancel();
            } else if (made_sprite::commitPrefetched(slot.stage)) {
                slot.provider = prefetch_provider_;
                slot.version = prefetch_version_;
                prefetch_fail_streak_ = 0;  // 成功即清连败计数，别把旧账算给下次
                ESP_LOGI(BUTTON_TAG, "[sprite] neighbor_staged slot=%+d provider=%s",
                    slot.side, slot.provider.c_str());
            }
            prefetch_provider_.clear();
            prefetch_version_ = 0;
            prefetch_slot_ = -1;
        } else if (!made_sprite::prefetchInFlight()) {
            // 解码失败：清登记走退避，稍后重试。
            const std::string failed_provider = prefetch_provider_;
            const uint32_t failed_version = prefetch_version_;
            made_sprite::prefetchCancel();
            prefetch_provider_.clear();
            prefetch_version_ = 0;
            prefetch_slot_ = -1;
            // ①退避必须在这里武装：prefetchBegin 成功而异步解码失败时，
            //   原实现只在 Begin 同步失败分支赋 prefetch_retry_ms_，这里
            //   没赋 → 下一帧（~150ms）立刻重试，文件缺失时变成每秒 6 次
            //   的 png_begin_failed 风暴（2026-10-19 dantai wings-0 实测）。
            prefetch_retry_ms_ = lv_tick_get() + 1000;
            // ②缓存残缺自愈：同一 provider+version 连败 ≥5 次（约 5s）=
            //   manifest 版本一致但文件丢了（快路径只信版本号发现不了），
            //   借用主台 cached_apply_dead 的 purge 通道让 worker 整套重下。
            if (prefetch_fail_provider_ == failed_provider &&
                prefetch_fail_version_ == failed_version) {
                ++prefetch_fail_streak_;
            } else {
                prefetch_fail_provider_ = failed_provider;
                prefetch_fail_version_ = failed_version;
                prefetch_fail_streak_ = 1;
            }
            if (prefetch_fail_streak_ >= 5 && !artwork_purge_pending_.load()) {
                std::lock_guard<std::mutex> purge_lock(artwork_mutex_);
                if (!artwork_purge_pending_.load()) {
                    artwork_purge_provider_ = failed_provider;
                    artwork_purge_pending_.store(true);
                    prefetch_fail_streak_ = 0;
                    ESP_LOGW(BUTTON_TAG,
                        "[sprite] prefetch_cache_dead provider=%s -> purge+redownload",
                        failed_provider.c_str());
                }
            }
        }
    }
    // 两侧都要补时按最近的拖动方向排优先级：方向侧先飞，另一侧靠后。
    if (drag_side_ == 1) { ensureSide(1); ensureSide(0); }
    else { ensureSide(0); ensureSide(1); }
}

// LVGL 线程。切换裁决出目标且目标侧台已挂好正确素材：三舞台指针轮换——
// 目标侧台升主位，旧主舞台转对侧台（内容正好是新主的对侧邻居），远侧台
// 转目标侧原位（内容由随后的 reconcile 校对补齐；恰好 3 个助手时它也
// 正确——永不重新解码）。没就绪返回 false 交回缓存直换/worker 老路。
bool VibeCoding::tryPromotePrefetch(const std::string &provider)
{
    if (!sprite_stage_ || artwork_applied_ == provider) return false;
    int slot_idx = -1;
    for (int i = 0; i < 2; ++i)
        if (sides_[i].provider == provider) { slot_idx = i; break; }
    if (slot_idx < 0) return false;
    SideSlot &src = sides_[slot_idx];
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        const auto *entry = findProviderLocked(provider);
        if (!entry || !entry->sprite.valid() || entry->sprite.version != src.version) {
            // 目录刷新后素材版本变了：这台挂的是旧版本，作废走老路。
            made_sprite::clearArtwork(src.stage);
            src.provider.clear();
            src.version = 0;
            return false;
        }
    }
    const uint32_t promoted_version = src.version;
    SideSlot &dst = sides_[1 - slot_idx];
    // 三方轮换：src→主位，主→dst 位，dst 原舞台→src 位。素材全跟舞台走。
    lv_obj_t *old_main = sprite_stage_;
    const std::string old_provider = artwork_applied_;
    const uint32_t old_version = artwork_applied_version_;
    const std::string dst_provider = dst.provider;
    const uint32_t dst_version = dst.version;
    sprite_stage_ = src.stage;
    src.stage = dst.stage;
    dst.stage = old_main;
    src.provider = dst_provider;
    src.version = dst_version;
    dst.provider = old_provider;
    dst.version = old_version;
    lv_obj_set_x(sprite_stage_, 0);
    lv_obj_set_x(src.stage, src.side < 0 ? -made_screen_w() : made_screen_w());
    lv_obj_set_x(dst.stage, dst.side < 0 ? -made_screen_w() : made_screen_w());
    artwork_applied_ = provider;
    artwork_applied_version_ = promoted_version;
    ESP_LOGI(BUTTON_TAG, "[sprite] neighbor_promoted provider=%s", provider.c_str());
    return true;
}

// 素材应用与清理（LVGL 线程）。provider 切走时立刻撤下旧素材；
// 下载完成的素材在下一帧淡入（这里是直接换，圆屏上视觉上就是一次闪切）。
// 目录里某助手素材的版本号（找不到/无效返回 0）。轮换记录对侧台内容版本用。
uint32_t VibeCoding::spriteVersionOf(const std::string &provider)
{
    std::lock_guard<std::mutex> lock(model_mutex_);
    const auto *entry = findProviderLocked(provider);
    return (entry && entry->sprite.valid()) ? entry->sprite.version : 0;
}

void VibeCoding::maybeApplyArtwork(const std::string &selected_provider)
{
    if (!sprite_stage_) return;
    // 每帧：在飞解码挂上归属侧台 + 两侧台内容对齐相邻助手（三舞台轮换后
    // 远侧台、目录版本刷新都在这里自动补齐）。
    prefetchTick();
    // 行动系统：舞台内容每帧都可能变（换装/晋升/清空），在此对齐可用动作
    // 名单——变化即置脏，worker 线程负责上报。
    syncAvailableActions();
    if (tryPromotePrefetch(selected_provider)) return;
    bool dirty = false;
    std::string ready;
    {
        std::lock_guard<std::mutex> lock(artwork_mutex_);
        dirty = artwork_dirty_.exchange(false);
        ready = artwork_ready_provider_;
        if (dirty) {
            // worker 刚提交了新缓存（下载完成或校验通过）：清掉直换的
            // 「同版本只试一次」记忆。否则首次加载失败记下的 memo 会永久
            // 屏蔽直换，之后每次切回都退回慢路闪空白。
            cached_apply_provider_.clear();
            cached_apply_version_ = 0;
        }
    }
    if (artwork_applied_ != selected_provider) {
        // 换装瞬态预算（随机快滑压测实锤）：解码新主台期间旧主台必须驻留
        // （commitArt 失败要回滚），若两侧台同时驻留，瞬态 = 旧3+新3+侧5
        // = 11 个大帧 > 槽池 8 → free 崩到 100KB、png_alloc_failed 连环。
        // 异步阀（prefetchTick 512KB）在同一拍内来不及救，必须在解码决策
        // 点同步清侧台，把瞬态封顶为旧+新主台 6 个大帧；换装落定后
        // ensureSide 会自动补台。free<1.2MB ≈ 侧台驻留态的水位。
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 1200 * 1024) {
            for (SideSlot &slot : sides_) {
                if (!slot.stage || slot.provider.empty()) continue;
                made_sprite::clearArtwork(slot.stage);
                slot.provider.clear();
                slot.version = 0;
                slot.defer_until = 0;
            }
        }
        if (made_sprite::prefetchInFlight()) {
            // 后台预取正在解码（单飞，通常就是本目标的侧台素材）：等一拍再
            // 试，绝不在 LVGL 线程并行再解一套——两套解码像素叠加正是 PSRAM
            // 峰值坑。worker 发布后下一帧 prefetchTick 挂侧台、晋升零解码；
            // worker 失败则落到下面的老路。
            return;
        }
        // 缓存直换：本地 manifest 版本与目录一致就直接整体换装（commitArt
        // 原子换容器，旧素材仍在显示时不闪空白），不再等 worker 的 ensure()
        // 逐文件 SPIFFS 读 + SHA-256 复核——那要好几秒，实机表现为「切到
        // 旺财先显示空白，过几秒才换装」。缓存的完整性不变量是 manifest
        // 最后写、写前每个文件都已验过哈希，版本一致即可信。
        if (tryApplyCachedArtwork(selected_provider)) {
            artwork_applied_ = selected_provider;
            artwork_applied_version_ = spriteVersionOf(selected_provider);
            return;
        }
        if (dirty || made_sprite::artworkActive(sprite_stage_)) {
            made_sprite::clearArtwork(sprite_stage_);
            artwork_applied_.clear();
        }
    }
    if (!dirty || ready.empty() || ready != selected_provider || artwork_applied_ == ready) return;
    std::string manifest;
    if (made_sprite_cache::loadManifest(selected_provider, manifest) &&
        made_sprite::applyArtwork(sprite_stage_, manifest, made_sprite_cache::baseDir(selected_provider))) {
        artwork_applied_ = selected_provider;
        artwork_applied_version_ = spriteVersionOf(selected_provider);
        ESP_LOGI(BUTTON_TAG, "[sprite] applied provider=%s", selected_provider.c_str());
        {   // 同 tryApplyCachedArtwork：应用成功即撤销本 provider 的陈旧 purge
            std::lock_guard<std::mutex> lock(artwork_mutex_);
            if (artwork_purge_provider_ == selected_provider) {
                artwork_purge_provider_.clear();
                artwork_purge_pending_.store(false);
            }
        }
    } else {
        made_sprite::clearArtwork(sprite_stage_);
        artwork_applied_.clear();
        if (made_sprite::lastApplyFail() == made_sprite::ApplyFail::alloc) {
            // 同 tryApplyCachedArtwork：alloc 失败≠缓存坏，丢预取腾内存等重试。
            made_sprite::prefetchCancel();
        }
        ESP_LOGI(BUTTON_TAG, "[sprite] apply_failed provider=%s", selected_provider.c_str());
    }
}

// LVGL 线程。同一条流水线的快速通道：读缓存 manifest 的 version，与目录
// 下发的一致就立即解码应用。同一 (provider, version) 只试一次（记忆在
// cached_apply_*），失败或没缓存就回退 worker 校验下载的老路，
// PNG 损坏等落盘级损坏会在 applyArtwork 解码失败时暴露出来走回退。
bool VibeCoding::tryApplyCachedArtwork(const std::string &provider)
{
    uint32_t version = 0;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        const vibe_provider::Provider *entry = findProviderLocked(provider);
        if (!entry || !entry->sprite.valid()) return false;
        version = entry->sprite.version;
    }
    {
        std::lock_guard<std::mutex> lock(artwork_mutex_);
        const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if (cached_apply_provider_ == provider && cached_apply_version_ == version) {
            if (cached_apply_attempts_ >= 2) {
                // 同版本连败 2 次：缓存大概率损坏（预加载只比对 manifest
                // 版本号，文件本身坏了发现不了）。请 worker 整套删除重下；
                // LVGL 侧停止反复解码——否则每 300ms 一次的全量 SPIFFS 读
                // + PNG 解码会把 LVGL 线程打满，截图/命令全部饿死。
                if (!artwork_purge_pending_.load()) {
                    artwork_purge_provider_ = provider;
                    artwork_purge_pending_.store(true);
                    ESP_LOGW(BUTTON_TAG,
                        "[sprite] cached_apply_dead provider=%s version=%u -> purge+redownload",
                        provider.c_str(), static_cast<unsigned>(version));
                }
                return false;
            }
            if (static_cast<int32_t>(now - cached_retry_at_ms_) < 300) return false;
        } else {
            cached_apply_attempts_ = 0;
        }
        cached_retry_at_ms_ = now;
    }
    uint32_t cached = 0;
    std::string manifest;
    if (!made_sprite_cache::cachedVersion(provider, cached) || cached != version ||
        !made_sprite_cache::loadManifest(provider, manifest)) {
        // 缓存未就绪不是错误：worker 正在下载/校验，300ms 后会自动再试，
        // worker 提交（dirty）时也会立即清 memo 解锁。
        std::lock_guard<std::mutex> lock(artwork_mutex_);
        cached_apply_provider_ = provider;
        cached_apply_version_ = version;
        return false;
    }
    if (!made_sprite::applyArtwork(sprite_stage_, manifest, made_sprite_cache::baseDir(provider))) {
        const bool alloc_fail =
            made_sprite::lastApplyFail() == made_sprite::ApplyFail::alloc;
        ESP_LOGW(BUTTON_TAG, "[sprite] cached_apply_failed provider=%s version=%u alloc=%d psram_free=%u",
            provider.c_str(), static_cast<unsigned>(version), alloc_fail ? 1 : 0,
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        std::lock_guard<std::mutex> lock(artwork_mutex_);
        cached_apply_provider_ = provider;
        cached_apply_version_ = version;
        if (alloc_fail) {
            // 内存不足/碎片：缓存文件没坏，不计数——否则两次后误判 dead
            // 触发 purge+redownload 死循环（实机每 2.7s 删了重下一整套）。
            // 丢预取帧腾最大连续块 + 拉长退避（applyArtwork 里已主动腾过，
            // 这里兜底在飞预取），1.5s 后重试。
            made_sprite::prefetchCancel();
            cached_retry_at_ms_ = static_cast<uint32_t>(esp_timer_get_time() / 1000) + 1200;
        } else {
            ++cached_apply_attempts_;
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(artwork_mutex_);
        cached_apply_provider_.clear();  // 成功即清：切走再切回仍可秒换
        cached_apply_version_ = 0;
        cached_apply_attempts_ = 0;
        // 成功即撤销本 provider 待执行的 purge：purge 决策来自下载窗口期的
        // 陈旧失败（manifest 先到、PNG 未齐被判缓存坏），现在缓存已被证明
        // 是好的，再 purge 就是误删刚下好的整套素材（2026-10-24 雪球实锤：
        // 应用成功 4s 后陈旧 purge 把新缓存删掉，设备又白屏一轮）。
        if (artwork_purge_provider_ == provider) {
            artwork_purge_provider_.clear();
            artwork_purge_pending_.store(false);
        }
    }
    ESP_LOGI(BUTTON_TAG, "[sprite] cached_applied provider=%s version=%u",
        provider.c_str(), static_cast<unsigned>(version));
    deferSidesForDecodePeak();
    return true;
}

// 换装错峰（LVGL 线程）：主舞台刚解码完整套素材（峰值已过但分配器还没
// 归还干净），立刻补两侧台就是三套解码 + 烘焙同窗叠加，开机时序最挤的
// 几秒会把 PSRAM 打到低谷（2026-10-18 排查：psram_free 曾跌到 658KB）。
// 侧台推迟 2.5s 再补，用户无感（侧台不在视野内）；拖动方向侧不受
// defer 限制（ensureSide 的 defer 条件对 drag_side_ 例外），不影响翻页。
void VibeCoding::deferSidesForDecodePeak()
{
    const uint32_t until = lv_tick_get() + 2500;
    for (auto &slot : sides_) slot.defer_until = until;
}

} // namespace esp_brookesia::apps
