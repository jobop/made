// SPDX-License-Identifier: Apache-2.0
// 远程命令模块：桥接器下发命令的接收（pullBoardCommands + ACK 重试）、
// 语义分发（applyRemoteCommands）、ui.swipe/ui.tap 远程操控，以及
// debug.heap 堆诊断。命令字段校验全部是纯函数，可单独单元测试。
#include "esp_brookesia_app_vibe.hpp"
#include "vibe_ui_shared.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_heap_task_info.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "made_sprite.hpp"
#include "new_message_chime.hpp"
#include "board_voice_audio.h"

namespace esp_brookesia::apps {

namespace {

// debug.heap 命令：dump 堆诊断。需要 CONFIG_HEAP_TASK_TRACKING=y 才有
// per-task 表——用它把「heap 越用越少」点名到具体线程。
void logHeapDiagnosticsImpl()
{
    ESP_LOGW("vibe_heap",
        "[heap] psram free=%u largest=%u min=%u | internal free=%u largest=%u min=%u",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    // 注意：CONFIG_HEAP_TASK_TRACKING=y 在 IDF v5.5.5 上开机即崩
    // （heap_caps_update_per_task_info_alloc 早期中断分配空指针，
    // 实机 2026-10-19 复现），任务级表保持关闭，这里只出池级统计。
}

std::string trimNotice(std::string caption)
{
    if (caption.size() > 120) caption.resize(120);
    while (!caption.empty() && (static_cast<unsigned char>(caption.back()) & 0xC0) == 0x80) caption.pop_back();
    if (!caption.empty() && (static_cast<unsigned char>(caption.back()) & 0xC0) == 0xC0) caption.pop_back();
    return caption;
}

std::string commandField(const cJSON *command, const char *key)
{
    const cJSON *fields = cJSON_GetObjectItemCaseSensitive(command, "fields");
    return cJSON_IsObject(fields) ? jsonString(fields, key) : std::string();
}

int parseLevel(const std::string &level)
{
    if (level.empty() || level.size() > 3) return -1;
    if (level.size() > 1 && level[0] == '0') return -1;
    int value = 0;
    for (char c : level) {
        if (c < '0' || c > '9') return -1;
        value = value * 10 + (c - '0');
    }
    return value <= 100 ? value : -1;
}

bool validThemeName(const std::string &name)
{
    if (name == "default") return true;
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

bool validDirection(const std::string &direction)
{
    return direction == "next" || direction == "prev" ||
           direction == "left" || direction == "right" ||
           direction == "up" || direction == "down";
}

// ui.tap 命中测试：返回 (x,y) 处最深的可见可点击对象（顶层子对象优先），无则空。
lv_obj_t *findClickableAt(lv_obj_t *obj, const lv_point_t &point)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || !lv_obj_is_visible(obj)) return nullptr;
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    if (point.x < coords.x1 || point.x > coords.x2 || point.y < coords.y1 || point.y > coords.y2) return nullptr;
    const int32_t count = lv_obj_get_child_count(obj);
    for (int32_t i = count - 1; i >= 0; --i) {
        lv_obj_t *hit = findClickableAt(lv_obj_get_child(obj, i), point);
        if (hit) return hit;
    }
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) ? obj : nullptr;
}

} // namespace

void VibeCoding::handleUiSwipe(const std::string &direction)
{
    if (!canOperate() || settings_open_.load()) return;
    if (direction == "left") {
        drawer_open_.load() ? chooseSession(1) : chooseProvider(1);
    } else if (direction == "right") {
        drawer_open_.load() ? chooseSession(-1) : chooseProvider(-1);
    } else if (direction == "up") {
        if (!drawer_open_.load()) showDrawer(true);
    } else if (direction == "down") {
        if (drawer_open_.load()) showDrawer(false);
    }
}

// ui.tap：命中 (x,y) 处最深可见可点击对象并派发 CLICKED（本应用全部点击回调都挂该事件）。
// 调试/插件自动化用途；不做按下-抬起全流程模拟。
void VibeCoding::handleUiTap(const std::string &x_text, const std::string &y_text)
{
    if (!canOperate() || settings_open_.load()) return;
    if (x_text.empty() || x_text.size() > 4 || y_text.empty() || y_text.size() > 4) return;
    for (const char c : x_text + y_text) {
        if (c < '0' || c > '9') return;
    }
    const int x = atoi(x_text.c_str());
    const int y = atoi(y_text.c_str());
    lv_obj_t *screen = lv_screen_active();
    if (!screen || x < 0 || y < 0) return;
    const lv_point_t point{x, y};
    lv_obj_t *target = findClickableAt(screen, point);
    if (target) lv_obj_send_event(target, LV_EVENT_CLICKED, nullptr);
}

void VibeCoding::applyRemoteCommands()
{
    std::vector<RemoteCommand> batch;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        batch.swap(inbound_commands_);
    }
    cmd_calls_.fetch_add(batch.size());
    for (auto &command : batch) {
        if (command.name == "caption.show") {
            std::string caption = trimNotice(command.text);
            if (caption.empty()) continue;
            std::lock_guard<std::mutex> lock(model_mutex_);
            model_.session_notice = std::move(caption);
            ++model_.revision;
        } else if (command.name == "audio.play") {
            if (!command.audio.empty() &&
                !made_chime_play_wav(reinterpret_cast<const uint8_t *>(command.audio.data()), command.audio.size())) {
                std::lock_guard<std::mutex> lock(model_mutex_);
                model_.session_notice = T("无法播放下发的语音");
                ++model_.revision;
            }
        } else if (command.name == "theme.apply") {
            if (validThemeName(command.theme)) startThemeApply(command.theme);
        } else if (command.name == "volume.set") {
            const int level = parseLevel(command.level);
            if (level >= 0) setVolumeLevel(level);
        } else if (command.name == "agent.select") {
            if (vibe_provider::validId(command.target)) selectProviderById(command.target);
            else if (command.direction == "next") chooseProvider(1);
            else if (command.direction == "prev") chooseProvider(-1);
        } else if (command.name == "session.create") {
            requestNewSession();
        } else if (command.name == "session.select") {
            if (validSessionId(command.target)) selectSessionById(command.target);
            else if (command.direction == "next") chooseSession(1);
            else if (command.direction == "prev") chooseSession(-1);
        } else if (command.name == "session.delete") {
            if (validSessionId(command.target)) queueDeleteSessionId(command.target);
        } else if (command.name == "voice.start") {
            startVoice();
        } else if (command.name == "task.confirm" || command.name == "task.cancel") {
            std::string task_id = command.target;
            if (task_id.empty()) {
                std::lock_guard<std::mutex> lock(model_mutex_);
                task_id = visible_task_id_;
            }
            if (validTaskId(task_id)) {
                queueActionForId(command.name == "task.confirm" ? "confirm" : "cancel", task_id);
            }
        } else if (command.name == "debug.screenshot") {
            startScreenshotUpload();
        } else if (command.name == "debug.heap") {
            logHeapDiagnosticsImpl();
        } else if (command.name == "action.play" || command.name.rfind("action.", 0) == 0) {
            // 行动系统：动作即命令（action.<名>，或兼容旧 action.play+fields.action）。
            // 两级绑定模型：命令名通常是抽象命令号（action.A1…），playAction
            // 按 manifest 的 commands 表映射到真实动作；素材没映射就静默
            // 忽略（降级不播，不报错）。
            std::string action = command.action;
            if (command.name.size() > 7) action = command.name.substr(7);
            if (!action.empty() && action.size() <= 16)
                made_sprite::playAction(sprite_stage_, action.c_str());
        } else if (command.name == "ui.swipe") {
            handleUiSwipe(command.direction);
        } else if (command.name == "ui.tap") {
            handleUiTap(command.x, command.y);
        } else if (command.name == "ui.back") {
            // 远程返回键：与实体返回键同一条 back() 路径（设置面板逐层退出）。
            // 故意不走 settings_open_ 拦截，否则设置页打开时远程无法恢复。
            back();
        }
    }
}

void VibeCoding::pullBoardCommands()
{
    auto postAck = [this](const std::vector<std::string> &ids) {
        if (ids.empty()) return true;
        std::string body = "{\"ids\":[";
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i) body += ',';
            body += '"';
            body += ids[i];
            body += '"';
        }
        body += "]}";
        std::string response;
        int status = 0;
        return request("/device/commands/ack", true, response, status, body) && status == 200;
    };
    if (!ack_retry_.empty() && postAck(ack_retry_)) ack_retry_.clear();
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (inbound_commands_.size() >= 8) return;
    }
    std::string response;
    int status = 0;
    if (!request("/device/commands", false, response, status) || status != 200) return;
    cJSON *root = cJSON_Parse(response.c_str());
    if (!root) return;
    const cJSON *commands = cJSON_GetObjectItemCaseSensitive(root, "commands");
    std::vector<RemoteCommand> accepted;
    std::vector<std::string> ack_ids = ack_retry_;
    if (cJSON_IsArray(commands)) {
        int count = 0;
        cJSON *command = nullptr;
        cJSON_ArrayForEach(command, commands) {
            if (count++ >= 8) break;
            if (!cJSON_IsObject(command)) continue;
            const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(command, "id");
            const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(command, "name");
            if (!cJSON_IsString(id_item) || !validCommandId(id_item->valuestring) || !cJSON_IsString(name_item)) continue;
            const std::string id = id_item->valuestring;
            if (std::find(ack_ids.begin(), ack_ids.end(), id) == ack_ids.end()) ack_ids.push_back(id);
            if (std::find(delivered_command_ids_.begin(), delivered_command_ids_.end(), id) != delivered_command_ids_.end()) continue;
            RemoteCommand next;
            next.id = id;
            next.name = name_item->valuestring;
            next.text = commandField(command, "text");
            next.target = commandField(command, "id");
            next.direction = commandField(command, "direction");
            next.level = commandField(command, "level");
            next.theme = commandField(command, "name");
            next.x = commandField(command, "x");
            next.y = commandField(command, "y");
            next.action = commandField(command, "action");
            if (!validDirection(next.direction)) next.direction.clear();
            const cJSON *audio = cJSON_GetObjectItemCaseSensitive(command, "audio");
            if (cJSON_IsTrue(audio)) {
                std::string clip;
                int clip_status = 0;
                const std::string path = "/device/commands/" + id + "/audio";
                if (!request(path, false, clip, clip_status, "", 256 * 1024, 35000) || clip_status != 200 || clip.size() < 44) {
                    ack_ids.erase(std::remove(ack_ids.begin(), ack_ids.end(), id), ack_ids.end());
                    continue;
                }
                next.audio = std::move(clip);
            }
            delivered_command_ids_.push_back(id);
            if (delivered_command_ids_.size() > 32) delivered_command_ids_.erase(delivered_command_ids_.begin());
            accepted.push_back(std::move(next));
        }
    }
    cJSON_Delete(root);
    if (!accepted.empty()) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        for (auto &command : accepted) {
            if (inbound_commands_.size() >= 8) {
                delivered_command_ids_.erase(std::remove(delivered_command_ids_.begin(), delivered_command_ids_.end(), command.id),
                                             delivered_command_ids_.end());
                ack_ids.erase(std::remove(ack_ids.begin(), ack_ids.end(), command.id), ack_ids.end());
                continue;
            }
            inbound_commands_.push_back(std::move(command));
        }
    }
    if (!postAck(ack_ids)) ack_retry_ = std::move(ack_ids);
    else ack_retry_.clear();
}

} // namespace esp_brookesia::apps
