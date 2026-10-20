// SPDX-License-Identifier: Apache-2.0
// 板侧事件模块：设备向桥接器上报的事件通道。事件入队（带订阅门禁）、
// 助手范围装填、能力清单重报（动作作为命令条目并入）、burn-in 观察通道
// 优先冲刷。双击手势（touch.double 事件源）也在此模块。
#include "esp_brookesia_app_vibe.hpp"
#include "vibe_ui_shared.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "esp_log.h"
#include "lvgl.h"
#include "made_sprite.hpp"
#include "vibe_pairing.hpp"

namespace esp_brookesia::apps {

// 行动系统（LVGL 线程，maybeApplyArtwork 每帧调用）：读当前素材舞台的
// 可用动作名单，与已上报的比对，变了就置 board_catalog_sent_ 为假等 worker
// 重报整张清单——动作作为命令条目（action.<名>）并入清单，不单开通道。
// 名单的真源是素材包 manifest：声明了 commands 表的素材上报命令号（A1–A9，
// 「命令绑素材」由素材作者定义），否则上报 actions 语义名（老素材兼容）。
void VibeCoding::syncAvailableActions()
{
    std::vector<std::string> names = made_sprite::actionNames(sprite_stage_);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        changed = names != available_actions_;
        if (changed) available_actions_ = std::move(names);
    }
    if (changed) {
        board_catalog_sent_ = false;   // 动作条目并入命令清单，变了整张重报
        std::string joined;
        for (const auto &name : names) {
            if (!joined.empty()) joined += ',';
            joined += name;
        }
        ESP_LOGI("vibe_actions", "[actions] list_changed count=%u list=%s",
            static_cast<unsigned>(names.size()), joined.c_str());
    }
}

void VibeCoding::queueBoardEvent(const char *name,
                                 const std::vector<std::pair<std::string, std::string>> &fields)
{
    if (!name || !name[0]) return;
    std::lock_guard<std::mutex> lock(model_mutex_);
    if (std::find(board_events_.begin(), board_events_.end(), name) == board_events_.end()) return;
    if (!pending_board_event_.empty()) return;
    pending_board_event_ = name;
    pending_board_event_fields_ = fields;
}

// 摇一摇（vibe_motion 检测任务回调）：当前精灵页助手 = 选中的助手，切页
// 瞬间 selected_provider_ 就已更新，不等心跳。把助手 ID 塞进事件字段，
// 让桥接器插件按助手范围决定要不要下发动作。
void VibeCoding::notifyMotionShake()
{
    queueAssistantScopedEvent("motion.shake");
}

// 助手范围事件的公共装填：把当前精灵页助手 ID 塞进 fields 再入队（订阅
// 门禁在 queueBoardEvent 内部，没有插件订阅该事件时这里安静返回）。
void VibeCoding::queueAssistantScopedEvent(const char *name)
{
    std::vector<std::pair<std::string, std::string>> fields;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        std::string agent = selectedProviderLocked().id;
        if (vibe_provider::validId(agent)) fields.emplace_back("agent", std::move(agent));
    }
    queueBoardEvent(name, fields);
}

// 助手页双击（屏幕连续两次短按）：本板 I2C 总线上无 IMU（2026-10-18
// 全总线扫描只有 codec/touch，摇一摇硬件上不可实现），双击是插件侧
// shake-fall 的等价触发源。只在首页正常态生效；点在按钮/抽屉/弹窗等
// 子对象上的 SHORT_CLICKED 不会冒泡到 screen，天然不误触。
void VibeCoding::homeShortClickCallback(lv_event_t *event)
{
    auto *self = static_cast<VibeCoding *>(lv_event_get_user_data(event));
    if (!self || lv_event_get_code(event) != LV_EVENT_SHORT_CLICKED) return;
    if (!self->canOperate() || self->settings_open_.load() ||
        self->drawer_open_.load() || self->delete_dialog_open_.load())
        return;
    lv_obj_t *screen = lv_scr_act();
    // 只在首页（精灵页）：拖动中或拖动刚结束的松手都不算点击。
    if (self->home_dragging_ || lv_obj_get_x(screen) != 0) return;
    const uint32_t now = lv_tick_get();
    if (self->last_home_tap_ms_ != 0 && now - self->last_home_tap_ms_ <= 450) {
        self->last_home_tap_ms_ = 0;
        self->queueAssistantScopedEvent("touch.double");
    } else {
        self->last_home_tap_ms_ = now;
    }
}

void VibeCoding::announceBoardCatalog()
{
    // 动作作为命令条目并入清单：每个可用条目一条 action.<名>。设备自己决定
    // 支持哪些（两级绑定模型，docs/SPRITES.md）：素材 manifest 声明了
    // commands 表的，上报抽象命令号（A1–A9，事件绑定只发命令号）；没声明
    // 的老素材上报动作语义名（弃用中，向后兼容）。名单变化时调用方会重新
    // 上报整张清单。只输出符合桥接器 CAPABILITY_NAME 的名字（字母开头的
    // [A-Za-z0-9_]{0,15}），不合规的静默跳过，避免整张清单被拒。
    std::vector<std::string> actions;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        actions = available_actions_;
    }
    static constexpr const char *head =
        "{\"events\":["
        "{\"name\":\"boot.click\",\"cadence\":\"edge\"},"
        "{\"name\":\"boot.double\",\"cadence\":\"edge\"},"
        "{\"name\":\"boot.triple\",\"cadence\":\"edge\"},"
        "{\"name\":\"motion.shake\",\"cadence\":\"edge\"},"
        "{\"name\":\"touch.double\",\"cadence\":\"edge\"}"
        "],\"commands\":["
        "{\"name\":\"caption.show\"},"
        "{\"name\":\"audio.play\"},"
        "{\"name\":\"theme.apply\"},"
        "{\"name\":\"volume.set\"},"
        "{\"name\":\"agent.select\"},"
        "{\"name\":\"session.create\"},"
        "{\"name\":\"session.select\"},"
        "{\"name\":\"session.delete\"},"
        "{\"name\":\"voice.start\"},"
        "{\"name\":\"task.confirm\"},"
        "{\"name\":\"task.cancel\"},"
        "{\"name\":\"debug.screenshot\"},"
        "{\"name\":\"debug.heap\"},"
        "{\"name\":\"ui.swipe\"},"
        "{\"name\":\"ui.tap\"},"
        "{\"name\":\"ui.back\"}";
    std::string body = head;
    auto valid_action = [](const std::string &name) {
        if (name.empty() || name.size() > 16) return false;
        // 抽象命令号（A1–A9）或老语义名（小写字母开头 [a-z0-9_]{0,15}）
        const bool slot = name.size() == 2 && name[0] == 'A' && name[1] >= '1' && name[1] <= '9';
        if (!slot && (name[0] < 'a' || name[0] > 'z')) return false;
        for (const char c : name) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_';
            if (!ok) return false;
        }
        return true;
    };
    for (const auto &name : actions) {
        if (!valid_action(name)) continue;
        body += ",{\"name\":\"action.";
        body += name;
        body += "\"}";
    }
    body += "]}";
    std::string response;
    int status = 0;
    if (!request("/device/capabilities", true, response, status, body) || status != 200) return;
    board_catalog_sent_ = true;
    announced_actions_ = std::move(actions);   // worker only
}

void VibeCoding::flushBoardEvent()
{
    std::string body;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        // burn-in 观察通道优先：绕过订阅门禁直发（桥接器打日志后拒收未订阅
        // 事件，无副作用）。观察期没有别的插件事件源，挤占无害。
        if (!burnin_event_body_.empty()) {
            body = std::move(burnin_event_body_);
            burnin_event_body_.clear();
        }
    }
    if (body.empty()) {
        std::string name;
        std::vector<std::pair<std::string, std::string>> fields;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            if (pending_board_event_.empty()) return;
            if (std::find(board_events_.begin(), board_events_.end(), pending_board_event_) == board_events_.end()) {
                pending_board_event_.clear();
                pending_board_event_fields_.clear();
                return;
            }
            name = std::move(pending_board_event_);
            pending_board_event_.clear();
            fields = std::move(pending_board_event_fields_);
            pending_board_event_fields_.clear();
        }
        body = std::string("{\"name\":\"") + name + "\"";
        if (!fields.empty()) {
            body += ",\"fields\":{";
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i) body += ',';
                body += '"'; body += fields[i].first; body += "\":\"";
                for (const char c : fields[i].second) {   // JSON 字符串转义（保守）
                    if (c == '"' || c == '\\') body += '\\';
                    body += c;
                }
                body += '"';
            }
            body += '}';
        }
        body += '}';
    }
    std::string response;
    int status = 0;
    (void)request("/device/events", true, response, status, body);
}

} // namespace esp_brookesia::apps
