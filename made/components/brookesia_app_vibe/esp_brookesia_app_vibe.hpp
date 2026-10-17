// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "esp_http_client.h"
#include "provider_catalog.hpp"
#include "vibe_wifi.hpp"
#include "vibe_theme.hpp"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace vibe_pairing { struct Snapshot; }
namespace vibe_phone_setup { struct Submission; }

namespace esp_brookesia::apps {

// Independent Coding Agent app: provider-specific tasks, voice capture, and
// deliberate BOOT/touch confirmation or cancellation through the PC bridge.
class VibeCoding final : public systems::phone::App {
public:
    static VibeCoding *requestInstance();
    static bool startOnDisplay();
    ~VibeCoding() override = default;

    // 回复区富内容附件：图片/链接二维码，桥接器下发或固件从 result 文本回退解析。
    struct Attachment {
        std::string type;   // "image" | "link"
        std::string url;    // 图片地址 / 链接地址
        std::string text;   // 链接展示文案（可选）
    };

protected:
    VibeCoding();
    bool run() override;
    bool pause() override;
    bool resume() override;
    bool back() override;
    bool close() override;

private:
    struct Session {
        std::string id;
        std::string title;
        std::string provider;
        std::string project_id;
    };
    struct Task {
        std::string id;
        std::string instruction;
        std::string provider;
        std::string project;
        std::string status;
        std::string result;
        std::string progress;
        std::string error;
        std::string session_id;
        bool result_truncated = false;
        bool instruction_truncated = false;
        std::vector<Attachment> attachments;  // 桥接器解析/下发，或固件从 result 文本回退解析
    };
    struct PendingAction {
        std::string id;
        std::string verb;
    };
    struct Snapshot {
        std::vector<Session> sessions;
        std::vector<Task> tasks;
        std::string connection_error;
        std::string feedback;
        std::string feedback_task_id;
        std::string voice_hint;
        std::string session_notice;
        bool action_in_flight = false;
        bool feedback_external = false;
        uint32_t revision = 0;
    };

    static VibeCoding *_instance;
    static void timerCallback(lv_timer_t *timer);
    void openForeground();
    bool canOperate() const { return active_.load() && !exit_requested_.load(); }
    static void displayDiagnostic(lv_event_t *event);
    static void providerSwipeCallback(lv_event_t *event);
    static void sessionSwipeCallback(lv_event_t *event);
    static void homeDragCallback(lv_event_t *event); // 翻屏跟手：按下/拖动/松手
    static void attachmentTapCallback(lv_event_t *event); // 点击附件→放大浮层
    static void zoomCloseCallback(lv_event_t *event);     // 点击浮层→关闭
    static std::vector<Attachment> parseUrlAttachments(const std::string &text);
    static std::vector<Attachment> parseAttachments(const cJSON *job); // 桥接器显式下发的 attachments
    // 富内容渲染与异步拉图：图片解码后驻留 PSRAM，浮层复用同一份 dsc。
    void freeDecodedImages();
    void renderAttachments(const std::vector<Attachment> &atts);
    void openZoom(size_t index);
    void startImageFetch(const std::string &url, lv_obj_t *target_img, size_t index);
    std::vector<uint8_t> fetchUrlBytes(const std::string &url, size_t max);
public:
    // 后台线程解码完成后在 LVGL 线程回调：建 dsc 并写入 decoded/attachment 列表，设置到目标 image。
    void onImageFetched(size_t index, uint8_t *px, int w, int h, lv_obj_t *img);
private:
    static void newSessionCallback(lv_event_t *event);
    static void deleteSessionCallback(lv_event_t *event);
    static void deleteSessionConfirmCallback(lv_event_t *event);
    static void deleteSessionCancelCallback(lv_event_t *event);
    static void sessionTitleTapCallback(lv_event_t *event);
    static void settingsCallback(lv_event_t *event);
    static void languageCallback(lv_event_t *event);
    static void languageOpenCallback(lv_event_t *event);
    static void languageBackCallback(lv_event_t *event);
    void refreshLanguage();
    static void settingsCloseCallback(lv_event_t *event);
    static void accessOpenCallback(lv_event_t *event);
    static void accessBackCallback(lv_event_t *event);
    static void accessPickerCallback(lv_event_t *event);
    static void accessModeCallback(lv_event_t *event);
    static void accessFieldCallback(lv_event_t *event);
    static void accessEditorDoneCallback(lv_event_t *event);
    static void accessEditorCancelCallback(lv_event_t *event);
    void closeAccessEditor(bool save);
    static void receiverScanOpenCallback(lv_event_t *event);
    static void receiverScanBackCallback(lv_event_t *event);
    static void receiverScanRefreshCallback(lv_event_t *event);
    static void receiverSelectCallback(lv_event_t *event);
    void showReceiverScan(bool visible);
    void renderReceiverScan();
    static void themeOpenCallback(lv_event_t *event);
    static void themeBackCallback(lv_event_t *event);
    static void themeSelectCallback(lv_event_t *event);
    static void themeSyncCallback(lv_event_t *event);
    static void themeRebootCallback(lv_timer_t *timer);
    void showTheme(bool visible);
    void renderThemes();
    static void powerOpenCallback(lv_event_t *event);
    static void powerBackCallback(lv_event_t *event);
    static void powerTimeoutCallback(lv_event_t *event);
    static void screenWakeCallback(lv_event_t *event);
    void showPower(bool visible);
    void renderPower();
    static void volumeOpenCallback(lv_event_t *event);
    static void volumeBackCallback(lv_event_t *event);
    static void volumeDownCallback(lv_event_t *event);
    static void volumeUpCallback(lv_event_t *event);
    void showVolume(bool visible);
    void renderVolume();
    void adjustVolume(int delta);
    void loadScreenOffTimeout();
    void applyScreenOff(bool off);
    static void wifiScanOpenCallback(lv_event_t *event);
    static void wifiScanBackCallback(lv_event_t *event);
    static void wifiScanRefreshCallback(lv_event_t *event);
    static void wifiNetworkCallback(lv_event_t *event);
    void showWifiScan(bool visible);
    void renderWifiNetworks();
    void startWifiScan();
    void connectSelectedWifi();
    static void phoneSetupOpenCallback(lv_event_t *event);
    static void phoneSetupBackCallback(lv_event_t *event);
    void showPhoneSetup(bool visible);
    void renderPhoneSetup();
    bool applyPhoneSetup(const vibe_phone_setup::Submission &submission, std::string &error);
    static void accessHttpsCallback(lv_event_t *event);
    static void accessSaveCallback(lv_event_t *event);
    static void bridgeRefreshCallback(lv_event_t *event);
    static void bridgeSelectCallback(lv_event_t *event);
    static void workerEntry(void *arg);
    static void buttonEntry(void *arg);
    static esp_err_t httpEvent(esp_http_client_event_t *event);

    void workerLoop();
    void buttonLoop();
    void ensureWorkerStarted();
    void chooseProvider(int direction);
    void chooseSession(int direction);
    void requestNewSession();
    void deleteSession(const std::string &provider_id, const std::string &session_id);
    void showDeleteSessionDialog(bool visible);
    void showSettings(bool visible);
    void showDrawer(bool visible);
    static void drawerSwipeCallback(lv_event_t *event);
    void showAccess(bool visible);
    void showLanguage(bool visible);
    void updateAccessModeButtons();
    void renderBridgeChoices(const vibe_pairing::Snapshot &pairing);
    void createSession(const std::string &provider_id, const std::string &project_id);
    const vibe_provider::Provider &selectedProviderLocked() const;
    vibe_provider::Provider *findProviderLocked(const std::string &id);
    void refreshSessions();
    void refreshTasks();
    void refreshConfig();
    void syncArtwork();
    void preloadArtworkCache();
    bool artworkRetryDue() const;
    bool tryApplyCachedArtwork(const std::string &provider);
    void maybeApplyArtwork(const std::string &selected_provider);
    void invalidateCatalog(bool clear = false);
    void sendHeartbeat();
    void sendAction(PendingAction action);
    bool request(const std::string &path, bool post, std::string &response, int &status,
                 const std::string &json_body = {}, size_t response_limit = 32 * 1024,
                 int timeout_ms = 5000);
    void queueBoardEvent(const char *name);
    void announceBoardCatalog();
    void flushBoardEvent();
    void pullBoardCommands();
    void applyRemoteCommands();
    void setVolumeLevel(int level);
    void selectProviderById(const std::string &id);
    void selectSessionById(const std::string &id);
    void queueDeleteSessionId(const std::string &id);
    void startThemeApply(const std::string &name);
    void startScreenshotUpload();
    void handleUiSwipe(const std::string &direction);
    void handleUiTap(const std::string &x_text, const std::string &y_text);
    void setConnectionError(std::string error);
    void queueActionForId(const char *verb, const std::string &task_id);
    void queueSubmittedVoiceCancel(const std::string &task_id);
    void startVoice(bool hold_mode = false, bool allow_record = true);
    void showAnswer(const std::string &scope, const std::string &text);
    void render();
    void layoutReadableText();
    void applyDrawerAnswerLayout(); // 回显区抽屉坐标（配对布局退出后必须恢复，否则叠进指令行）
    void renderStatusBadge(const std::string &status); // 空串=隐藏
    void setActivityRunning(bool running);             // 活动条循环扫动动画开关

    std::mutex model_mutex_;
    // 形象素材同步状态：worker 线程负责下载（syncArtwork），LVGL 线程负责
    // 解码与显示（maybeApplyArtwork）；两边只通过 artwork_dirty_ 握手。
    std::mutex artwork_mutex_;
    std::string artwork_target_;
    uint32_t artwork_target_version_ = 0;
    bool artwork_attempted_ = false;
    // 素材同步失败后的重试计划（毫秒时间戳，0=无需重试）。瞬时 USB/网络抖动
    // 不该把助手永久留在回退形象上（2026-10-14 实测：一次失败即永久豆子）。
    std::atomic<uint32_t> artwork_retry_at_ms_{0};
    uint32_t artwork_retry_count_ = 0;  // 仅 worker 线程 + artwork_mutex_ 内访问
    std::string artwork_ready_provider_;
    std::atomic<bool> artwork_dirty_{false};
    std::atomic<bool> artwork_request_{false};
    std::string artwork_applied_;  // 仅 LVGL 线程访问
    uint32_t artwork_applied_version_ = 0;  // 与 artwork_applied_ 同步更新
    std::string artwork_seen_provider_;  // 仅 LVGL 线程访问
    // 缓存直换的记忆（tryApplyCachedArtwork，仅 LVGL 线程 + artwork_mutex_）：
    // 只在失败时记录，作为 300ms 退避（避免未就绪时每帧读 SPIFFS）；
    // 成功即清——同一助手切走再切回必须能再次秒换，绝不能锁死直换路径。
    std::string cached_apply_provider_;
    uint32_t cached_apply_version_ = 0;
    uint32_t cached_retry_at_ms_ = 0;
    int cached_apply_attempts_ = 0;  // 同版本连败计数；≥2 触发 purge 重下
    // LVGL 判定缓存损坏后的整套重下请求（worker 线程消费）。
    std::atomic<bool> artwork_purge_pending_{false};
    std::string artwork_purge_provider_;  // artwork_mutex_ 内读写
    // 开机预加载（仅 worker 线程 + model_mutex_）：目录刷新后把所有带素材的
    // 助手排队，每轮循环下载一个（见 preloadArtworkCache），滑动切换时
    // tryApplyCachedArtwork 直接命中本地缓存秒换，不用现等下载。
    struct ArtworkPreloadEntry {
        std::string provider;
        uint32_t version = 0;
        uint8_t attempts = 0;
    };
    std::vector<ArtworkPreloadEntry> artwork_preload_queue_;
    uint64_t artwork_preload_epoch_ = 0;
    // 三舞台轮换（仅 LVGL 线程访问）：主舞台 + 左右侧台各一套。实测三套
    // 解码像素合计 ~2.2MB（猫 0.59/鸟 0.47/狗 1.11），8MB PSRAM 放得下。
    // 侧台常驻解码好的相邻助手（停靠屏外 ±1 屏宽，拖动平移时随屏滑入
    // 视野——真翻页）；松手晋升只是三指针轮换（旧主舞台转对侧台，内容
    // 正好是新主的对侧邻居；远侧台内容由 reconcile 校对补齐）——恰好
    // 3 个助手时永不重新解码，任何方向随时可翻。
    //
    // 助手 >3 个时轮换不自洽：每次同向晋升后远侧台要补解码新邻居，而
    // 此刻 4 套像素并存会挤爆 PSRAM（实测第 4 套起飞时 free 常 <1.5MB，
    // prefetchBegin 直接拒绝 → 每秒退避空转 → 第三屏永不预载）。解法是
    // PSRAM 预算回收：方向侧起飞前余量不足就回收背向侧台（用户背对它，
    // 暂时用不到），腾出空间给新邻居；方向反转时背向侧变方向侧，照样
    // 回收对侧、重建自己。
    struct SideSlot {
        lv_obj_t *stage = nullptr;
        std::string provider;      // 该侧台当前挂的素材（空=无）
        uint32_t version = 0;
        int side = 0;              // -1 屏左 / +1 屏右（座位固定，不随轮换变）
        uint32_t defer_until = 0;  // 被回收让位后的暂缓补齐截止时刻（0=不缓）
    };
    SideSlot sides_[2];
    int drag_side_ = -1;                   // 最近一次拖动的方向侧台（0/1，-1=无）；补齐顺序优先它
    std::string prefetch_provider_;        // 在飞解码的目标（空=无）
    uint32_t prefetch_version_ = 0;
    int prefetch_slot_ = -1;               // 在飞解码归属的侧台（0/1）
    uint32_t prefetch_retry_ms_ = 0;       // 起飞失败/解码失败的退避重试时刻
    void maybeStartPrefetch(int direction);
    void prefetchTick();                   // 每帧：解码完挂上归属侧台+对齐两侧
    void ensureSide(int index);            // 侧台内容对齐相邻助手（按需解码）
    bool tryPromotePrefetch(const std::string &provider);  // 命中则三指针轮换
    bool sideTarget(int index, std::string &target, uint32_t &version);
    uint32_t spriteVersionOf(const std::string &provider); // 目录里素材的版本号
    std::mutex voice_action_mutex_; // Serializes BOOT/touch voice actions with app pause/close.
    Snapshot model_;
    PendingAction pending_action_;
    bool pending_create_session_ = false; // Protected by model_mutex_.
    bool create_session_in_flight_ = false; // Protected by model_mutex_.
    std::string pending_create_provider_id_; // Protected by model_mutex_.
    std::string pending_create_project_id_; // Protected by model_mutex_.
    bool pending_delete_session_ = false; // Protected by model_mutex_.
    bool delete_session_in_flight_ = false; // Protected by model_mutex_.
    std::string pending_delete_provider_id_; // Protected by model_mutex_.
    std::string pending_delete_session_id_; // Protected by model_mutex_.
    std::string delete_dialog_provider_id_; // LVGL task only.
    std::string delete_dialog_session_id_; // LVGL task only.
    std::atomic<bool> active_{false};
    std::atomic<bool> worker_started_{false};
    std::atomic<bool> button_started_{false};
    std::atomic<uint32_t> button_epoch_{0};
    std::atomic<int> selected_provider_{0};
    std::atomic<bool> voice_available_{false};
    std::atomic<bool> config_loaded_{false};
    std::atomic<bool> language_refresh_pending_{false};
    std::atomic<uint32_t> catalog_epoch_{0};
    std::atomic<bool> exit_requested_{false};
    std::atomic<bool> settings_open_{false};
    std::atomic<bool> drawer_open_{false};
    std::atomic<bool> phone_setup_requested_{false};
    std::atomic<uint32_t> phone_setup_epoch_{0};
    std::atomic<uint32_t> phone_setup_ready_epoch_{UINT32_MAX};
    std::atomic<int> phone_setup_result_{0}; // 1 = saved; -1 = error. Worker -> LVGL.
    std::atomic<bool> restore_station_pending_{false};
    std::atomic<bool> foreground_network_pending_{false};
    std::string phone_setup_error_; // Protected by model_mutex_.
    std::atomic<bool> delete_dialog_open_{false};
    std::atomic<uint32_t> ui_tick_ms_{0};
    std::atomic<uint32_t> ui_stage_{0};
    std::atomic<uint32_t> boot_tick_ms_{0};
    std::atomic<uint32_t> display_stage_{0};
    std::atomic<uint32_t> display_count_{0};
    bool display_diag_registered_ = false;
    std::atomic<bool> submitted_task_seen_{false};
    std::string visible_task_id_; // Protected by model_mutex_; used by BOOT task.
    std::string voice_session_id_; // Protected by model_mutex_; tags voice status to its session.
    std::string voice_project_id_; // Protected by model_mutex_.
    std::vector<std::string> board_events_; // Protected by model_mutex_. Subscribed event names.
    std::string pending_board_event_; // Protected by model_mutex_. At most one report in flight.
    struct RemoteCommand {
        std::string id;
        std::string name;
        std::string text;
        std::string target;
        std::string direction;
        std::string level;
        std::string theme;
        std::string x;
        std::string y;
        std::string audio;
    };
    std::vector<RemoteCommand> inbound_commands_; // Protected by model_mutex_.
    std::vector<std::string> delivered_command_ids_; // Worker only.
    std::vector<std::string> ack_retry_; // Worker only.
    bool board_catalog_sent_ = false; // Worker only; cleared when the catalog is invalidated.
    std::vector<vibe_provider::Provider> providers_; // Protected by model_mutex_; starts empty.
    std::string catalog_identity_; // Protected by model_mutex_; URL + authenticated token.

    // LVGL objects are accessed exclusively from the LVGL task.
    lv_obj_t *sprite_stage_ = nullptr;
    uint32_t sprite_started_ms_ = 0;
    // 精灵瞬时情绪（成功/失败）：盯最前面的任务，状态从进行中翻到终态时
    // 播一小段表情。仅 LVGL 线程访问。
    std::string sprite_watch_id_;
    std::string sprite_watch_status_;
    int sprite_transient_mood_ = -1;          // made_sprite::Mood 值，-1=无
    uint32_t sprite_transient_until_ = 0;     // lv_tick_get() 毫秒
    lv_obj_t *drawer_ = nullptr;
    lv_obj_t *drawer_grip_ = nullptr;
    lv_obj_t *drawer_hint_ = nullptr;
    lv_obj_t *provider_label_ = nullptr;
    bool rendered_catalog_ready_ = false;
    bool pairing_drawer_auto_ = false;   // 配对期间自动拉开的抽屉，配对完成后自动收囘
    lv_obj_t *page_label_ = nullptr;
    lv_obj_t *new_session_button_ = nullptr;
    lv_obj_t *delete_session_button_ = nullptr;
    lv_obj_t *delete_dialog_ = nullptr;
    lv_obj_t *delete_dialog_title_ = nullptr;
    lv_obj_t *settings_button_ = nullptr;
    lv_obj_t *settings_overlay_ = nullptr;
    lv_obj_t *settings_home_ = nullptr;
    // 任务详情重设计：分页 chip（左上）+ 状态徽章（右上）+ 不确定式活动条。
    // 徽章「点+文字」双通道传状态；活动条只表达"进行中"，任务链路无百分比。
    lv_obj_t *session_chip_ = nullptr;
    lv_obj_t *session_chip_label_ = nullptr;
    lv_obj_t *status_badge_ = nullptr;
    lv_obj_t *status_badge_dot_ = nullptr;
    lv_obj_t *status_badge_label_ = nullptr;
    lv_obj_t *activity_bar_ = nullptr;
    lv_obj_t *activity_fill_ = nullptr;
    bool activity_anim_running_ = false; // LVGL task only.
    lv_obj_t *language_panel_ = nullptr;
    lv_obj_t *language_zh_button_ = nullptr;
    lv_obj_t *language_en_button_ = nullptr;
    lv_obj_t *language_auto_button_ = nullptr;
    lv_obj_t *language_hint_ = nullptr;
    uint32_t last_locale_revision_ = UINT32_MAX;
    lv_obj_t *access_panel_ = nullptr;
    lv_obj_t *access_save_button_ = nullptr;
    lv_obj_t *access_change_button_ = nullptr;
    lv_obj_t *access_change_label_ = nullptr;
    lv_obj_t *access_picker_hint_ = nullptr;
    lv_obj_t *phone_setup_button_ = nullptr;
    lv_obj_t *phone_setup_panel_ = nullptr;
    lv_obj_t *phone_setup_qr_ = nullptr;
    lv_obj_t *phone_setup_hint_ = nullptr;
    lv_obj_t *phone_setup_network_ = nullptr;
    lv_obj_t *phone_setup_password_ = nullptr;
    lv_obj_t *phone_setup_address_ = nullptr;
    uint32_t phone_setup_revision_ = UINT32_MAX;
    lv_obj_t *access_auto_button_ = nullptr;
    lv_obj_t *access_manual_button_ = nullptr;
    lv_obj_t *access_receiver_button_ = nullptr;
    lv_obj_t *access_usb_button_ = nullptr;
    lv_obj_t *access_usb_hint_ = nullptr;
    lv_obj_t *access_host_input_ = nullptr;
    lv_obj_t *access_port_input_ = nullptr;
    lv_obj_t *access_password_input_ = nullptr;
    lv_obj_t *access_https_button_ = nullptr;
    lv_obj_t *access_https_label_ = nullptr;
    lv_obj_t *access_keyboard_ = nullptr;
    lv_obj_t *access_editor_panel_ = nullptr;
    lv_obj_t *access_editor_input_ = nullptr;
    lv_obj_t *access_editor_title_ = nullptr;
    lv_obj_t *access_editor_target_ = nullptr;
    lv_obj_t *access_status_label_ = nullptr;
    lv_obj_t *receiver_scan_button_ = nullptr;
    lv_obj_t *receiver_picker_ = nullptr;
    lv_obj_t *receiver_list_ = nullptr;
    lv_obj_t *receiver_hint_ = nullptr;
    lv_obj_t *receiver_refresh_button_ = nullptr;
    struct ReceiverRow { lv_obj_t *button; std::string ssid; };
    std::vector<ReceiverRow> receiver_rows_;
    uint32_t receiver_scan_revision_ = UINT32_MAX;
    lv_obj_t *receiver_pressed_button_ = nullptr;
    lv_point_t receiver_press_point_{};
    bool receiver_press_moved_ = false;
    lv_obj_t *wifi_picker_ = nullptr;
    lv_obj_t *wifi_network_list_ = nullptr;
    lv_obj_t *wifi_scan_status_ = nullptr;
    lv_obj_t *wifi_password_input_ = nullptr;
    vibe_wifi::StationScanSnapshot wifi_scan_snapshot_; // LVGL task only.
    lv_obj_t *theme_panel_ = nullptr;
    lv_obj_t *theme_rows_ = nullptr;
    lv_obj_t *theme_status_label_ = nullptr;
    std::string theme_rows_signature_; // LVGL task only.
    std::vector<vibe_theme::ThemeInfo> theme_live_list_; // 桥接器实时列表（model_mutex_ 保护）。
    std::atomic<bool> theme_list_loading_{false};
    std::atomic<bool> screenshot_busy_{false};
    std::atomic<uint32_t> screenshot_busy_since_ms_{0}; // 看门狗：卡死超过阈值强制复位。
    std::atomic<bool> theme_reboot_pending_{false};
    std::atomic<bool> theme_reboot_started_{false};
    int32_t theme_loading_since_ms_ = 0; // LVGL task only.
    lv_obj_t *power_panel_ = nullptr;
    lv_obj_t *power_timeout_button_ = nullptr;
    lv_obj_t *power_timeout_label_ = nullptr;
    lv_obj_t *volume_panel_ = nullptr;
    lv_obj_t *volume_value_label_ = nullptr;
    lv_obj_t *power_off_overlay_ = nullptr;
    uint32_t screen_off_timeout_s_ = 0; // 0 = never.
    bool screen_off_ = false;
    uint32_t wifi_scan_list_revision_ = UINT32_MAX; // LVGL task only.
    bool wifi_join_pending_ = false; // LVGL task only.
    std::string wifi_join_ssid_; // LVGL task only.
    std::string wifi_selected_ssid_; // LVGL task only.
    bool access_manual_selected_ = false;
    bool access_picker_open_ = true;
    bool access_receiver_selected_ = false;
    bool access_usb_selected_ = false;
    bool access_https_selected_ = true;
    struct BridgeRow {
        lv_obj_t *button;
        std::string id;
        std::string url;
    };
    lv_obj_t *bridge_picker_ = nullptr;
    lv_obj_t *bridge_list_ = nullptr;
    lv_obj_t *bridge_hint_ = nullptr;
    lv_obj_t *bridge_refresh_button_ = nullptr;
    std::vector<BridgeRow> bridge_rows_;
    uint32_t bridge_list_revision_ = UINT32_MAX;
    bool bridge_wifi_online_ = false;
    lv_obj_t *bridge_pressed_button_ = nullptr;
    lv_point_t bridge_press_point_{};
    bool bridge_press_moved_ = false;
    lv_obj_t *position_label_ = nullptr;
    lv_obj_t *session_title_tap_ = nullptr;
    lv_obj_t *meta_label_ = nullptr;
    lv_obj_t *instruction_label_ = nullptr;
    lv_obj_t *answer_area_ = nullptr;
    lv_obj_t *result_label_ = nullptr;
    // 助手页顶部角标：左上状态点（绿在线/橙工作中/红离线），右上未读回显铃铛。
    lv_obj_t *status_dot_ = nullptr;
    lv_obj_t *unread_mark_ = nullptr;
    bool unread_reply_ = false;
    std::string answer_scope_;
    std::string answer_text_;
    bool showing_pairing_layout_ = false;
    std::string displayed_task_id_;
    std::string reply_watch_id_;
    bool reply_watch_ready_ = false;
    bool reply_saw_pending_ = false;
    bool reply_wait_new_ = false;
    // 横向翻屏跟手：拖动时整体平移 screen（素材是子对象，随屏一起走）。
    bool home_dragging_ = false;
    int drag_start_x_ = 0;
    int drag_start_y_ = 0;
    int drag_dx_ = 0;
    lv_coord_t screen_base_x_ = 0;
    uint32_t drag_end_ms_ = 0; // 抑制同一手势触发的重复 providerSwipe
    uint32_t last_pressing_ms_ = 0; // 按压心跳：PRESSING 断流=松手事件丢失，看门狗强制收尾
    // 回复区富内容（图片/链接二维码）与点击放大浮层。
    std::vector<lv_obj_t *> attachment_objs_;
    lv_obj_t *attachments_box_ = nullptr;
    std::vector<Attachment> current_attachments_;
    lv_obj_t *zoom_overlay_ = nullptr;
    std::vector<lv_image_dsc_t *> decoded_dscs_; // 异步解码结果，freeDecodedImages 释放
    std::vector<lv_image_dsc_t *> attachment_dscs_; // 与 current_attachments_ 平行，按索引回查已解码图
    std::string attachment_sig_; // 附件集合指纹，避免每次 render 重拉图/闪屏
    void noteComputerReply(const Task &task);
    void updateStatusIndicators(bool provider_ready);
    std::string rendered_session_id_;
    std::string last_voice_message_;
    int last_voice_phase_ = -1;
    uint32_t last_pair_revision_ = UINT32_MAX;
    uint32_t last_provider_swipe_ms_ = 0;
    uint32_t last_session_swipe_ms_ = 0;
    uint32_t drawn_revision_ = UINT32_MAX;
};

} // namespace esp_brookesia::apps
