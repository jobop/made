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
    ~VibeCoding() override = default;

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
    static void lockSwipeCallback(lv_event_t *event);
    void enterLockScreen();
    void unlockScreen();
    bool canOperate() const { return active_.load() && !locked_.load() && !exit_requested_.load(); }
    static void displayDiagnostic(lv_event_t *event);
    static void providerSwipeCallback(lv_event_t *event);
    static void sessionSwipeCallback(lv_event_t *event);
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
    void invalidateCatalog(bool clear = false);
    void showProviderIcon(const vibe_provider::Icon &icon);
    void sendHeartbeat();
    void sendAction(PendingAction action);
    bool request(const std::string &path, bool post, std::string &response, int &status,
                 const std::string &json_body = {});
    void queueBoardEvent(const char *name);
    void announceBoardCatalog();
    void flushBoardEvent();
    void setConnectionError(std::string error);
    void queueActionForId(const char *verb, const std::string &task_id);
    void queueSubmittedVoiceCancel(const std::string &task_id);
    void startVoice();
    void showAnswer(const std::string &scope, const std::string &text);
    void render();
    void layoutReadableText();

    std::mutex model_mutex_;
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
    std::atomic<bool> locked_{true};
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
    bool board_catalog_sent_ = false; // Worker only; cleared when the catalog is invalidated.
    std::vector<vibe_provider::Provider> providers_; // Protected by model_mutex_; starts empty.
    std::string catalog_identity_; // Protected by model_mutex_; URL + authenticated token.

    // LVGL objects are accessed exclusively from the LVGL task.
    lv_obj_t *lock_screen_ = nullptr;
    uint32_t lock_started_ms_ = 0;
    lv_point_t lock_touch_origin_{};
    bool lock_touch_tracking_ = false;
    lv_obj_t *provider_label_ = nullptr;
    lv_obj_t *animal_halo_ = nullptr;
    lv_obj_t *provider_image_ = nullptr;
    lv_obj_t *icon_placeholder_ = nullptr;
    lv_image_dsc_t provider_image_descriptor_{};
    alignas(4) std::array<uint8_t, vibe_provider::MAX_ICON_PIXELS * 4> provider_image_pixels_{};
    vibe_provider::Icon rendered_icon_;
    bool rendered_catalog_ready_ = false;
    lv_obj_t *page_label_ = nullptr;
    lv_obj_t *new_session_button_ = nullptr;
    lv_obj_t *delete_session_button_ = nullptr;
    lv_obj_t *delete_dialog_ = nullptr;
    lv_obj_t *delete_dialog_title_ = nullptr;
    lv_obj_t *settings_button_ = nullptr;
    lv_obj_t *settings_overlay_ = nullptr;
    lv_obj_t *settings_home_ = nullptr;
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
    lv_obj_t *wifi_scan_button_ = nullptr;
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
    std::string answer_scope_;
    std::string answer_text_;
    bool showing_pairing_layout_ = false;
    std::string displayed_task_id_;
    std::string reply_watch_id_;
    bool reply_watch_ready_ = false;
    bool reply_saw_pending_ = false;
    bool reply_wait_new_ = false;
    void noteComputerReply(const Task &task);
    std::string rendered_session_id_;
    std::string last_voice_message_;
    int last_voice_phase_ = -1;
    uint32_t last_pair_revision_ = UINT32_MAX;
    uint32_t last_provider_swipe_ms_ = 0;
    uint32_t last_session_swipe_ms_ = 0;
    uint32_t drawn_revision_ = UINT32_MAX;
};

} // namespace esp_brookesia::apps
