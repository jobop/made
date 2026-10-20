// SPDX-License-Identifier: Apache-2.0
// 渲染模块：render() 按屏幕状态拆出的视图函数。每个视图只管一种屏幕状态
// （精灵动效 / 配对 / 无助手 / 空会话 / 任务），共享上下文打包在
// RenderViewCtx（一次加锁快照，视图函数只读不改判定基准）。
// 职责边界：视图函数不做 revision 判定、不写 last_* 游标（归 render 主体）。
#include "esp_brookesia_app_vibe.hpp"
#include "vibe_ui_shared.hpp"

#include <algorithm>
#include <string>

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "made_layout.hpp"
#include "made_sprite.hpp"
#include "status_icons.h"
#include "vibe_i18n.hpp"
#include "vibe_pairing.hpp"
#include "vibe_voice_capture.hpp"

namespace esp_brookesia::apps {

// 各视图共享的只读上下文 RenderViewCtx 定义在 esp_brookesia_app_vibe.hpp
// （VibeCoding 嵌套结构），render() 主体一次构建，视图函数只消费。

// 精灵动效 + 瞬时情绪状态机 + 侧台同步（每帧必须跑，不受 revision 提前
// 返回影响——放在 render 主体 revision 检查之前）。
void VibeCoding::renderSpriteStage(const vibe_voice::Status &voice)
{
    if (!sprite_stage_) return;
    std::string task_status;
    std::string connection_error;
    std::string selected_provider;
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        connection_error = model_.connection_error;
        if (!model_.tasks.empty()) task_status = model_.tasks.front().status;
        selected_provider = selectedProviderLocked().id;
    }
    // 助手切换即刻反映到素材（撤旧、请求 worker 下载新的）。
    if (artwork_seen_provider_ != selected_provider) {
        artwork_seen_provider_ = selected_provider;
        artwork_request_.store(true);
        ESP_LOGI(BUTTON_TAG, "[sprite] provider_changed provider=%s -> request",
            selected_provider.c_str());
    }
    maybeApplyArtwork(selected_provider);
    // 瞬时情绪：盯最前面的任务，进行中 → 终态的翻转换来 ~3.5s 的
    // 成功/失败表情。素材没配对应 emotions 条目时，固件只是动效节奏
    // 略有不同，安全无害。
    {
        std::string watch_id;
        std::string watch_status;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            if (!model_.tasks.empty()) {
                watch_id = model_.tasks.front().id;
                watch_status = model_.tasks.front().status;
            }
        }
        if (watch_id != sprite_watch_id_) {
            sprite_watch_id_ = watch_id;
            sprite_watch_status_ = watch_status;
            sprite_transient_mood_ = -1;   // 新任务：旧表情立刻让位
        } else if (watch_status != sprite_watch_status_) {
            const bool was_pending = sprite_watch_status_ == "queued" ||
                sprite_watch_status_ == "running" ||
                sprite_watch_status_ == "waiting_confirmation";
            if (was_pending && watch_status == "completed") {
                sprite_transient_mood_ = made_sprite::Mood::kSuccess;
                sprite_transient_until_ = lv_tick_get() + 3500;
            } else if (was_pending && (watch_status == "failed" ||
                                       watch_status == "cancelled")) {
                sprite_transient_mood_ = made_sprite::Mood::kSad;
                sprite_transient_until_ = lv_tick_get() + 3500;
            }
            sprite_watch_status_ = watch_status;
        }
        const bool pending_now = watch_status == "queued" ||
            watch_status == "running" || watch_status == "waiting_confirmation";
        if (pending_now) sprite_transient_mood_ = -1;   // 又有活干：表情收起
    }
    int mood = made_sprite::Mood::kIdle;
    if (!connection_error.empty()) mood = made_sprite::Mood::kOffline;
    else if (voice.phase == vibe_voice::Phase::Recording || voice.phase == vibe_voice::Phase::Uploading)
        mood = made_sprite::Mood::kTalking;
    else if (sprite_transient_mood_ >= 0 &&
             static_cast<int32_t>(lv_tick_get() - sprite_transient_until_) < 0)
        mood = sprite_transient_mood_;
    else if (task_status == "waiting_confirmation") mood = made_sprite::Mood::kWaitingConfirm;
    else if (task_status == "running" || task_status == "queued") mood = made_sprite::Mood::kRunning;
    made_sprite::update(sprite_stage_, lv_tick_get() - sprite_started_ms_, mood);
    // 侧台同步驱动：拖动中滑入视野的素材也是活的（呼吸/眨眼/轻摆），
    // 不是一张死图。烘焙已对侧台关闭，走动态缩放路径。
    for (auto &slot : sides_)
        if (slot.stage)
            made_sprite::update(slot.stage, lv_tick_get() - sprite_started_ms_, mood);
    // 素材页顶部状态图标与精灵同一状态源（voice phase + 最前任务），
    // 随帧刷新；抽屉/设置打开时内部自隐藏。
    updateStatusIcon(static_cast<int>(voice.phase), task_status, !connection_error.empty());
}

// 配对视图：未配对/未授权期间抽屉自动拉开、六位配对码居中醒目。
void VibeCoding::renderPairingView(const RenderViewCtx &ctx)
{
    lv_obj_add_flag(new_session_button_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
    displayed_task_id_.clear();
    // 配对码显示在抽屉里：未配对期间保持抽屉拉开，用户随时能看到六位码。
    // 记住是这里自动拉开的，配对完成后自动收囘，开机不拐进抽屉。
    if (!drawer_open_.load()) {
        showDrawer(true);
        pairing_drawer_auto_ = true;
    }
    if (!showing_pairing_layout_) {
        lv_obj_remove_flag(meta_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(answer_area_, made_x(24), made_page_y(196));
        lv_obj_set_size(answer_area_, made_s(312), made_page_h(58));
        lv_obj_set_style_text_align(result_label_, LV_TEXT_ALIGN_CENTER, 0);
        // 配对版式沿用主页的居中排版（抽屉构建处已把这些行改成左对齐）。
        lv_obj_set_style_text_align(instruction_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_align(position_label_, LV_TEXT_ALIGN_CENTER, 0);
        showing_pairing_layout_ = true;
    }
    const size_t scheme = ctx.pairing.url.find("://");
    const std::string target = ctx.pairing.url.empty() ? std::string() :
        ctx.pairing.url.substr(scheme == std::string::npos ? 0 : scheme + 3);
    const bool usb_direct = ctx.pairing.access_mode == vibe_pairing::AccessMode::UsbDirect;
    // 位置行只在有独立信息时出现（USB 直连 / 已定目标）；否则与下面
    // 的引导行重复成两行「正在查找电脑」（实机截图 bug）。
    const std::string computer = usb_direct ? T("USB 直连电脑") :
        target.empty() ? std::string() : T("目标 ") + target;
    if (computer.empty()) {
        lv_obj_add_flag(position_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(position_label_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(position_label_, computer.c_str());
    }
    lv_label_set_text(instruction_label_, ctx.pairing.phase == vibe_pairing::Phase::WaitingApproval ?
                      T("六位配对码") : T("正在查找电脑"));
    lv_label_set_text(meta_label_, ctx.pairing.code.empty() ? "------" : ctx.pairing.code.c_str());
    const std::string pairing_message = ctx.snapshot.connection_error == "Connect receiver hotspot first" ?
        T("请核对接收端热点，确认接收端已插入电脑") :
        ctx.snapshot.connection_error == "Connect Wi-Fi first" ?
        T("请先在设置里连接 Wi-Fi") : ctx.pairing.phase == vibe_pairing::Phase::WaitingApproval ?
        T("在电脑页面核对号码并确认") : ctx.pairing.retry_required ? T("单击 BOOT 重新配对") : ctx.pairing.message;
    showAnswer("pairing", usb_direct || target.empty() ? pairing_message :
               T("目标：") + target + "\n" + pairing_message);
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        visible_task_id_.clear();
    }
    layoutReadableText();
    drawn_revision_ = ctx.snapshot.revision;
}

// 已配对但目录未就绪 / 没有任何助手的过渡视图。
void VibeCoding::renderCatalogEmptyView(const RenderViewCtx &ctx)
{
    lv_obj_add_flag(delete_session_button_, LV_OBJ_FLAG_HIDDEN);
    displayed_task_id_.clear();
    rendered_session_id_.clear();
    lv_label_set_text(position_label_, ctx.catalog_ready ? T("暂无助手") : T("读取助手列表"));
    lv_label_set_text(instruction_label_, ctx.catalog_ready ? T("请在电脑端添加助手插件") : T("正在读取电脑设置"));
    // 状态类短文案与上方居中的标题行同排版，避免「贴左孤行」的观感。
    lv_obj_set_style_text_align(result_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(instruction_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(position_label_, LV_TEXT_ALIGN_CENTER, 0);
    showAnswer("catalog", !ctx.snapshot.connection_error.empty() ? ctx.own_error :
        ctx.catalog_ready ? T("助手名称和图标将由电脑下发。") : T("连接成功后会自动显示可用助手。"));
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        visible_task_id_.clear();
    }
    layoutReadableText();
    drawn_revision_ = ctx.snapshot.revision;
}

// 已选助手但还没有任何会话的空状态视图。
void VibeCoding::renderNoSessionView(const RenderViewCtx &ctx)
{
    displayed_task_id_.clear();
    lv_obj_add_flag(session_chip_, LV_OBJ_FLAG_HIDDEN);
    renderStatusBadge("");
    setActivityRunning(false);
    lv_label_set_text(position_label_, T("暂无任务"));
    lv_label_set_text(instruction_label_, !ctx.snapshot.session_notice.empty() ? ctx.own_session_notice.c_str() : T("点 + 创建任务"));
    lv_obj_set_style_text_align(result_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(instruction_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(position_label_, LV_TEXT_ALIGN_CENTER, 0);
    const std::string empty_message = !ctx.snapshot.connection_error.empty() ? ctx.own_error :
        !ctx.snapshot.session_notice.empty() ? ctx.own_session_notice :
        !ctx.provider_ready ? (ctx.provider.reason.empty() ? T("请先在电脑端启用此助手") : ctx.provider.reason) :
        T("先创建任务，再按住 BOOT 说话。");
    showAnswer(ctx.provider.id + "/no-session", empty_message);
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        visible_task_id_.clear();
    }
    layoutReadableText();
    drawn_revision_ = ctx.snapshot.revision;
}

// 任务视图：chip+标题行+状态徽章+活动条+回显+富内容附件。
// 空任务列表与最新任务两条路径都在这里（外层已选定会话）。
void VibeCoding::renderTaskView(const RenderViewCtx &ctx, const Session *selected_session)
{
    if (ctx.snapshot.tasks.empty()) {
        displayed_task_id_.clear();
        renderStatusBadge("");
        setActivityRunning(false);
        lv_label_set_text(instruction_label_, !ctx.snapshot.session_notice.empty() ? ctx.own_session_notice.c_str() :
                          !ctx.voice_notice.empty() ? ctx.voice_notice.c_str() :
                          ctx.provider_ready ? T("还没有对话 · 按住 BOOT 说话") : T("电脑端助手不可用"));
        const std::string result = !ctx.snapshot.connection_error.empty() ? ctx.own_error :
            !config_loaded_.load() ? T("正在读取电脑设置") :
            !ctx.snapshot.session_notice.empty() ? ctx.own_session_notice :
            !voice_available_.load() ? T("在电脑工作台设置语音识别") :
            ctx.voice.phase == vibe_voice::Phase::Recording ?
                (ctx.voice.hold_mode ? T("按住说话中，松手即发送。") : T("说完后稍等，会自动发送。")) :
            ctx.voice.phase == vibe_voice::Phase::Uploading ? T("正在识别语音…") :
            !ctx.voice_notice.empty() ? ctx.voice_notice :
            ctx.provider.external_unscoped ? T("此助手任务隔离受限，请在电脑端核对。") :
            T("按住 BOOT 说话，发起新任务。");
        showAnswer(ctx.selected_session_id + "/empty", result);
        renderAttachments({}); // 无任务时清空可能残留的旧附件
    } else {
        // The bridge returns newest first. Show only the latest exchange.
        const Task &task = ctx.snapshot.tasks.front();
        displayed_task_id_ = task.id;
        // 回复监听的会话轨标识：助手 id + 会话 id。切助手/切会话时
        // 轨道变化，noteComputerReply 只登记不响铃（详见其注释）。
        noteComputerReply(task, ctx.provider.id + "/" + ctx.selected_session_id);
        // 状态已由徽章承载，指令行不再重复拼接状态词。
        const std::string instruction = !ctx.snapshot.session_notice.empty() ? ctx.own_session_notice :
            !ctx.voice_notice.empty() ? ctx.voice_notice :
            std::string(T("你：")) + task.instruction;
        renderStatusBadge(task.status);
        setActivityRunning(task.status == "running");
        std::string result;
        bool showing_task_result = false;
        if (task.status == "waiting_confirmation") {
            // The short header is only a preview. A user must be able to read
            // the entire recognized instruction before the BOOT double-click.
            const std::string notice = task.instruction_truncated ?
                T("任务过长，请在电脑任务页核对并确认") :
                !ctx.snapshot.connection_error.empty() ? ctx.own_error :
                (ctx.snapshot.feedback_task_id == task.id && !ctx.snapshot.feedback.empty()) ? ctx.own_feedback :
                !task.error.empty() ? task.error : T("双击 BOOT 确认 · 三击取消");
            result = notice + "\n" + (task.instruction.empty() ? T("（无任务内容）") : task.instruction);
        } else if (!ctx.snapshot.connection_error.empty()) result = ctx.own_error;
        else if (ctx.snapshot.feedback_task_id == task.id && !ctx.snapshot.feedback.empty()) result = ctx.own_feedback;
        else if (!task.error.empty()) result = task.error;
        else if (task.status == "running" && !task.progress.empty()) result = task.progress;
        else if (task.status == "queued") result = T("已排队，等待助手启动");
        else {
            result = task.result.empty() ? T("等待助手回复") : task.result;
            showing_task_result = true;
        }
        if (showing_task_result && task.result_truncated) {
            result += T("\n\n更多内容请在电脑任务页查看");
        }
        if (!ctx.snapshot.session_notice.empty()) result = ctx.own_session_notice + (result.empty() ? std::string() : "\n\n" + result);
        lv_label_set_text(instruction_label_, instruction.c_str());
        ui_stage_.store(30);
        showAnswer(ctx.selected_session_id + "/" + task.id + "/" + task.status,
                   result);
        // 富内容：仅在实际 AI 回显（非反馈/错误/排队）时渲染；其它状态清空旧附件。
        if (showing_task_result) {
            renderAttachments(task.attachments.empty() ? parseUrlAttachments(task.result)
                                                       : task.attachments);
        } else {
            renderAttachments({});
        }
    }
    {
        std::lock_guard<std::mutex> lock(model_mutex_);
        visible_task_id_ = displayed_task_id_;
        if (ctx.voice.phase == vibe_voice::Phase::Submitted && visible_task_id_ == ctx.voice.task_id) {
            submitted_task_seen_ = true;
        }
    }
    layoutReadableText();
    drawn_revision_ = ctx.snapshot.revision;
}

} // namespace esp_brookesia::apps
