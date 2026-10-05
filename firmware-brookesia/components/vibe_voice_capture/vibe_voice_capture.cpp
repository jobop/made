#include "vibe_i18n.hpp"
#include "vibe_voice_capture.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_voice_audio.h"
#include "pcm_upload_gain.hpp"
#include "tdm_mic_selector.hpp"
#include "vibe_pairing.hpp"
#include "vibe_usb.hpp"
#include "voice_activity_detector.hpp"

namespace vibe_voice {
namespace {

constexpr size_t kSamplesPerSecond = 16000;
constexpr size_t kMaxSamples = VoiceActivityDetector::max_samples;
constexpr size_t kMinSamples = kSamplesPerSecond / 2;
constexpr size_t kWavHeaderSize = 44;
constexpr size_t kResponseCapacity = 16 * 1024;
constexpr auto kOwner = BSP_EXTRA_AUDIO_OWNER_VIBE;
const char* kTag = "vibe_voice";

portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> busy{false};
std::atomic<bool> cancel_requested{false};
std::atomic<bool> usb_upload_active{false};
Phase current_phase = Phase::Idle;
char current_message[128] = "单击 BOOT 开始录音";
char current_task_id[41] = {};

struct Parameters {
    std::string url;
    std::string token;
    std::string provider;
    std::string project;
    std::string session;
};

struct Response {
    char* data = nullptr;
    size_t used = 0;
    bool truncated = false;
};

void set_status(Phase phase, const char* message) {
    char bounded[sizeof(current_message)] = {};
    std::snprintf(bounded, sizeof(bounded), "%s", message);
    portENTER_CRITICAL(&state_lock);
    current_phase = phase;
    std::memcpy(current_message, bounded, sizeof(current_message));
    portEXIT_CRITICAL(&state_lock);
}

bool valid_id(const std::string& value) {
    if (value.empty() || value.size() > 40) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-';
    });
}

bool valid_session_id(const std::string& value) {
    if (value.size() != 36) return false;
    for (size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

bool valid_task_id(const char* value) {
    if (value == nullptr) return false;
    const size_t length = std::strlen(value);
    if (length == 0 || length > 40) return false;
    for (size_t index = 0; index < length; ++index) {
        const char character = value[index];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') || character == '-')) return false;
    }
    return true;
}

void write_u16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void write_u32(uint8_t* out, uint32_t value) {
    for (int index = 0; index < 4; ++index) out[index] = static_cast<uint8_t>(value >> (8 * index));
}

void write_wav_header(uint8_t* wav, size_t samples) {
    const uint32_t bytes = static_cast<uint32_t>(samples * sizeof(int16_t));
    std::memcpy(wav, "RIFF", 4);
    write_u32(wav + 4, bytes + 36);
    std::memcpy(wav + 8, "WAVEfmt ", 8);
    write_u32(wav + 16, 16);
    write_u16(wav + 20, 1);
    write_u16(wav + 22, 1);
    write_u32(wav + 24, 16000);
    write_u32(wav + 28, 32000);
    write_u16(wav + 32, 2);
    write_u16(wav + 34, 16);
    std::memcpy(wav + 36, "data", 4);
    write_u32(wav + 40, bytes);
}

esp_err_t on_http_event(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA || event->user_data == nullptr || event->data_len <= 0) {
        return ESP_OK;
    }
    auto* response = static_cast<Response*>(event->user_data);
    const size_t remaining = kResponseCapacity - 1 - response->used;
    const size_t length = std::min(remaining, static_cast<size_t>(event->data_len));
    std::memcpy(response->data + response->used, event->data, length);
    response->used += length;
    response->data[response->used] = '\0';
    if (length < static_cast<size_t>(event->data_len)) response->truncated = true;
    return ESP_OK;
}

bool response_task_id(const Response& response, char* task_id, size_t capacity) {
    if (response.truncated || response.used == 0) return false;
    cJSON* root = cJSON_ParseWithLength(response.data, response.used);
    if (root == nullptr) return false;
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const char* value = cJSON_IsString(id) ? cJSON_GetStringValue(id) : nullptr;
    const bool valid = valid_task_id(value) && std::strlen(value) < capacity;
    if (valid) std::memcpy(task_id, value, std::strlen(value) + 1);
    cJSON_Delete(root);
    return valid;
}

bool cancel_submitted_task(const Parameters& parameters, const char* task_id) {
    if (!valid_task_id(task_id)) return false;
    const auto pairing = vibe_pairing::snapshot();
    if (!vibe_pairing::authorized_for_foreground() || pairing.url != parameters.url ||
        pairing.token != parameters.token) return false;
    const std::string path = vibe_i18n::request_path(std::string("/device/tasks/") + task_id + "/cancel");
    if (vibe_usb::is_direct_url(parameters.url)) {
        std::string response;
        int status_code = 0;
        const esp_err_t result = vibe_usb::request(
            path, true,
            "Bearer " + parameters.token, "application/json", nullptr, 0,
            response, status_code, kResponseCapacity - 1, 10000);
        return result == ESP_OK && status_code == 200;
    }
    if (parameters.url.rfind("https://", 0) == 0 && !vibe_pairing::tls_time_ready()) return false;
    const std::string url = parameters.url + path;
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.disable_auto_redirect = true;
    if (url.rfind("https://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 10000;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) return false;
    esp_http_client_set_header(client, "ngrok-skip-browser-warning", "1");
    const std::string authorization = "Bearer " + parameters.token;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_post_field(client, "", 0);
    const esp_err_t result = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK || status_code != 200) {
        ESP_LOGW(kTag, "Voice task cancellation failed: %s, HTTP %d",
                 esp_err_to_name(result), status_code);
        return false;
    }
    return true;
}

bool upload(const Parameters& parameters, const uint8_t* wav, size_t size,
            char* created_task_id, size_t task_id_capacity) {
    if (cancel_requested.load()) {
        set_status(Phase::Idle, "录音已取消");
        return false;
    }
    const auto pairing = vibe_pairing::snapshot();
    if (!vibe_pairing::authorized_for_foreground() || pairing.url != parameters.url ||
        pairing.token != parameters.token) {
        set_status(Phase::Error, "电脑连接已变化，请重新录音");
        return false;
    }
    if (parameters.url.rfind("https://", 0) == 0 && !vibe_pairing::tls_time_ready()) {
        set_status(Phase::Error, "设备时间未同步，无法校验 HTTPS 证书");
        return false;
    }
    std::string path = "/device/voice?provider=" + parameters.provider;
    if (!parameters.project.empty()) path += "&projectId=" + parameters.project;
    path += "&sessionId=" + parameters.session;
    path = vibe_i18n::request_path(path);
    const std::string url = parameters.url + path;
    std::unique_ptr<char, decltype(&heap_caps_free)> response_memory(
        static_cast<char*>(heap_caps_malloc(kResponseCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        &heap_caps_free);
    if (!response_memory) {
        set_status(Phase::Error, "上传内存不足");
        return false;
    }
    response_memory.get()[0] = '\0';
    Response response{response_memory.get(), 0, false};
    const std::string authorization = "Bearer " + parameters.token;
    esp_err_t result = ESP_FAIL;
    int status_code = 0;
    if (vibe_usb::is_direct_url(parameters.url)) {
        std::string usb_response;
        usb_upload_active.store(true);
        // cancel() can arrive while preparing the WAV or allocating response
        // memory. Close that race before the first USB request frame is sent.
        if (cancel_requested.load()) {
            usb_upload_active.store(false);
            set_status(Phase::Idle, "录音已取消");
            return false;
        }
        result = vibe_usb::request(path, true, authorization, "audio/wav", wav, size,
                                   usb_response, status_code, kResponseCapacity - 1, 160000,
                                   &cancel_requested);
        usb_upload_active.store(false);
        response.used = usb_response.size();
        std::memcpy(response.data, usb_response.data(), response.used);
        response.data[response.used] = '\0';
    } else {
        esp_http_client_config_t config = {};
        config.url = url.c_str();
        config.disable_auto_redirect = true;
        if (url.rfind("https://", 0) == 0) config.crt_bundle_attach = esp_crt_bundle_attach;
        config.method = HTTP_METHOD_POST;
        config.timeout_ms = pairing.access_mode == vibe_pairing::AccessMode::Receiver ? 160000 : 120000;
        config.event_handler = on_http_event;
        config.user_data = &response;
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            set_status(Phase::Error, "无法创建电脑连接");
            return false;
        }
        esp_http_client_set_header(client, "ngrok-skip-browser-warning", "1");
        esp_http_client_set_header(client, "Authorization", authorization.c_str());
        esp_http_client_set_header(client, "Content-Type", "audio/wav");
        esp_http_client_set_post_field(client, reinterpret_cast<const char*>(wav), static_cast<int>(size));
        if (cancel_requested.load()) {
            esp_http_client_cleanup(client);
            set_status(Phase::Idle, "录音已取消");
            return false;
        }
        result = esp_http_client_perform(client);
        status_code = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
    }
    if (result != ESP_OK || status_code != 201) {
        ESP_LOGW(kTag, "Voice upload failed: %s, HTTP %d, %.180s",
                 esp_err_to_name(result), status_code, response.data);
        if (cancel_requested.load() && vibe_usb::is_direct_url(parameters.url)) {
            set_status(Phase::Idle, "语音上传已取消");
            return false;
        }
        set_status(Phase::Error, cancel_requested.load() ? "取消状态未知，请检查任务列表" :
                   status_code == 400 ? "识别失败或未配置语音模型" :
                   "无法提交语音任务，请检查电脑连接");
        return false;
    }
    if (!response_task_id(response, created_task_id, task_id_capacity)) {
        set_status(Phase::Error, cancel_requested.load() ? "取消状态未知，请检查任务列表" :
                   "任务已提交，请在任务列表查看");
        return false;
    }
    portENTER_CRITICAL(&state_lock);
    std::memcpy(current_task_id, created_task_id, std::strlen(created_task_id) + 1);
    portEXIT_CRITICAL(&state_lock);
    if (cancel_requested.load()) {
        set_status(Phase::Uploading, "正在取消电脑任务");
    } else {
        set_status(Phase::Submitted, "已识别语音，任务等待确认");
    }
    return true;
}

void record_task(void* argument) {
    std::unique_ptr<Parameters> parameters(static_cast<Parameters*>(argument));
    uint8_t* wav = nullptr;
    bool audio_owned = false;
    size_t samples = 0;
    uint32_t mic1_max_level = 0;
    uint32_t mic2_max_level = 0;
    uint32_t mic2_blocks = 0;
    char created_task_id[41] = {};

    do {
        const esp_err_t acquired = bsp_extra_audio_session_acquire(kOwner);
        if (acquired != ESP_OK) {
            set_status(Phase::Error, "麦克风正被其他应用使用");
            break;
        }
        audio_owned = true;
        const esp_err_t initialized = bsp_extra_codec_set_voice_fs(
            16000, 16, CODEC_VOICE_INPUT_CHANNELS,
            BSP_EXTRA_ES7210_TDM_ALL_SLOTS_MASK,
            BSP_EXTRA_ES7210_PHYSICAL_CONNECTED_MIC_MASK);
        if (initialized != ESP_OK) {
            ESP_LOGW(kTag, "Microphone initialization failed: %s", esp_err_to_name(initialized));
            set_status(Phase::Error, "麦克风初始化失败");
            break;
        }
        ESP_LOGI(kTag, "Microphone opened: 16 kHz, 4 TDM slots");
        (void)bsp_extra_codec_mute_set(true);
        wav = static_cast<uint8_t*>(heap_caps_malloc(kWavHeaderSize + kMaxSamples * sizeof(int16_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (wav == nullptr) {
            set_status(Phase::Error, "录音内存不足");
            break;
        }
        auto* mono = reinterpret_cast<int16_t*>(wav + kWavHeaderSize);
        alignas(4) int16_t tdm[256 * 4] = {};
        VoiceActivityDetector detector;
        set_status(Phase::Recording, "正在听，请说话");
        while (!cancel_requested.load() && samples < kMaxSamples) {
            size_t bytes = 0;
            const esp_err_t read = bsp_extra_i2s_read(tdm, sizeof(tdm), &bytes, 250);
            if (read != ESP_OK || bytes == 0) {
                ESP_LOGW(kTag, "Microphone read failed: %s", esp_err_to_name(read));
                set_status(Phase::Error, "录音中断，请重试");
                break;
            }
            // ES7210 slots 0 and 2 are the physical microphones. XiaoZhi uses
            // both; pick the usable one for Vibe's mono transcription stream.
            const size_t frames = bytes / (4 * sizeof(int16_t));
            const size_t accepted_frames = std::min(frames, kMaxSamples - samples);
            if (accepted_frames == 0) continue;
            const size_t first_sample = samples;
            const auto levels = selectMic(tdm, accepted_frames, mono + first_sample);
            mic1_max_level = std::max(mic1_max_level, levels.mic1);
            mic2_max_level = std::max(mic2_max_level, levels.mic2);
            if (levels.selected_slot == 2) ++mic2_blocks;
            samples += accepted_frames;
            const auto event = detector.observe(mono + first_sample, samples - first_sample);
            if (event == VoiceActivityDetector::Event::SpeechStarted) {
                set_status(Phase::Recording, "已听到语音，停顿后自动发送");
            } else if (event == VoiceActivityDetector::Event::Complete) {
                break;
            } else if (event == VoiceActivityDetector::Event::NoSpeech) {
                ESP_LOGW(kTag, "No speech: MIC1 max level=%u, MIC2 max level=%u, MIC2 blocks=%u",
                         mic1_max_level, mic2_max_level, mic2_blocks);
                set_status(Phase::Error, "没听到语音，请单击 BOOT 重试");
                break;
            }
        }
        ESP_LOGI(kTag, "Capture finished: samples=%u, heard=%d, MIC1 max level=%u, MIC2 max level=%u, MIC2 blocks=%u",
                 static_cast<unsigned>(samples), detector.heardSpeech(),
                 mic1_max_level, mic2_max_level, mic2_blocks);
        (void)bsp_extra_audio_session_release(kOwner);
        audio_owned = false;
        if (cancel_requested.load()) {
            set_status(Phase::Idle, "录音已取消");
            break;
        }
        if (status().phase == Phase::Error) break;
        if (!detector.heardSpeech()) {
            set_status(Phase::Error, "没听到语音，请单击 BOOT 重试");
            break;
        }
        if (samples < kMinSamples) {
            set_status(Phase::Error, "请至少说半秒钟");
            break;
        }
        const uint32_t upload_gain = amplifyForUpload(mono, samples);
        ESP_LOGI(kTag, "Applying %ux digital gain to completed voice upload",
                 static_cast<unsigned>(upload_gain));
        write_wav_header(wav, samples);
        set_status(Phase::Uploading, "正在识别语音并提交任务");
        (void)upload(*parameters, wav, kWavHeaderSize + samples * sizeof(int16_t),
                     created_task_id, sizeof(created_task_id));
    } while (false);

    if (audio_owned) (void)bsp_extra_audio_session_release(kOwner);
    if (wav != nullptr) heap_caps_free(wav);
    bool cancel_attempted = false;
    for (;;) {
        // Serialize the final busy transition with cancel(): a late cancel after
        // the 201 response must not leave a waiting task behind.
        portENTER_CRITICAL(&state_lock);
        const bool need_remote_cancel = cancel_requested.load() &&
                                        created_task_id[0] != '\0' && !cancel_attempted;
        if (!need_remote_cancel) busy.store(false);
        portEXIT_CRITICAL(&state_lock);
        if (!need_remote_cancel) break;
        const bool cancelled = cancel_submitted_task(*parameters, created_task_id);
        cancel_attempted = true;
        set_status(cancelled ? Phase::Idle : Phase::Error,
                   cancelled ? "语音任务已取消" : "取消未确认，请在任务列表检查");
    }
    // Free heap-backed strings explicitly: vTaskDelete() does not unwind C++ stack objects.
    parameters.reset();
    vTaskDelete(nullptr);
}

}  // namespace

bool start(const std::string& bridge_url, const std::string& token,
           const std::string& provider, const std::string& project_id,
           const std::string& session_id) {
    if (busy.exchange(true)) return false;
    if ((bridge_url.rfind("http://", 0) != 0 && bridge_url.rfind("https://", 0) != 0) ||
        (bridge_url.rfind("https://", 0) == 0 && !vibe_pairing::tls_time_ready()) ||
        token.size() < 24 ||
        !valid_id(provider) || (!project_id.empty() && !valid_id(project_id)) ||
        !valid_session_id(session_id)) {
        busy.store(false);
        set_status(Phase::Error, "请先新建并选择会话");
        return false;
    }
    auto* parameters = new (std::nothrow) Parameters{bridge_url, token, provider, project_id, session_id};
    if (parameters == nullptr) {
        busy.store(false);
        set_status(Phase::Error, "录音内存不足");
        return false;
    }
    while (!parameters->url.empty() && parameters->url.back() == '/') parameters->url.pop_back();
    cancel_requested.store(false);
    ESP_LOGI(kTag, "Recording requested for provider=%s", provider.c_str());
    ESP_LOGW(kTag, "Memory before voice task: free=%lu largest_internal=%lu total_internal=%lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    portENTER_CRITICAL(&state_lock);
    current_task_id[0] = '\0';
    portEXIT_CRITICAL(&state_lock);
    set_status(Phase::Recording, "正在启动麦克风");
    if (xTaskCreate(record_task, "vibe_voice", 12288, parameters, 4, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "xTaskCreate(vibe_voice, 12288) failed: free=%lu largest_internal=%lu",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        delete parameters;
        busy.store(false);
        set_status(Phase::Error, "无法启动录音任务");
        return false;
    }
    return true;
}

void cancel() {
    portENTER_CRITICAL(&state_lock);
    if (busy.load()) {
        cancel_requested.store(true);
        if (current_phase == Phase::Recording) {
            std::snprintf(current_message, sizeof(current_message), "%s", "正在取消录音");
        } else if (current_phase == Phase::Uploading || current_phase == Phase::Submitted) {
            current_phase = Phase::Uploading;
            std::snprintf(current_message, sizeof(current_message), "%s", "正在取消电脑任务");
        }
    }
    portEXIT_CRITICAL(&state_lock);
    if (usb_upload_active.load()) vibe_usb::cancel_current();
}

Status status() {
    char message[sizeof(current_message)] = {};
    char task_id[sizeof(current_task_id)] = {};
    Phase phase;
    portENTER_CRITICAL(&state_lock);
    phase = current_phase;
    std::memcpy(message, current_message, sizeof(message));
    std::memcpy(task_id, current_task_id, sizeof(task_id));
    portEXIT_CRITICAL(&state_lock);
    return {phase, vibe_i18n::message(message), task_id};
}

}  // namespace vibe_voice
