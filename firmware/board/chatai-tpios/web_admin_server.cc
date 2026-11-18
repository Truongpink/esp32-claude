#include "web_admin_server.h"

#include "board.h"
#include "display/display.h"
#include "audio_codec.h"
#include "application.h"
#include "wifi_board.h"
#include "settings.h"
#include "gemini_live_protocol.h"
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <wifi_manager.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>
#include <string>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <esp_attr.h>

extern const char index_html_start[] asm("_binary_wifi_configuration_html_start");

namespace {
constexpr char TAG[] = "WebAdmin";
constexpr size_t kMaxBody = 8192;
constexpr size_t kOtaChunkSize = 1024;
constexpr size_t kMaxSystemPromptBytes = 3900;
httpd_handle_t server = nullptr;
std::mutex server_mutex;
std::mutex ota_mutex;

const char* ResetReasonLabel(esp_reset_reason_t reason);

constexpr uint32_t kRuntimeLogMagic = 0x434C4F47;
constexpr size_t kRuntimeLogCapacity = 4096;
struct RuntimeLogStore {
    uint32_t magic;
    uint32_t write_index;
    uint32_t total_bytes;
    char data[kRuntimeLogCapacity];
};
__NOINIT_ATTR RuntimeLogStore runtime_log_store;
portMUX_TYPE runtime_log_mux = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t original_log_vprintf = nullptr;
bool runtime_log_capture_started = false;

void EnsureRuntimeLogStore() {
    portENTER_CRITICAL(&runtime_log_mux);
    if (runtime_log_store.magic != kRuntimeLogMagic ||
        runtime_log_store.write_index >= kRuntimeLogCapacity) {
        std::memset(&runtime_log_store, 0, sizeof(runtime_log_store));
        runtime_log_store.magic = kRuntimeLogMagic;
    }
    portEXIT_CRITICAL(&runtime_log_mux);
}

void AppendRuntimeLog(const char* data, size_t length) {
    if (data == nullptr || length == 0) return;
    portENTER_CRITICAL(&runtime_log_mux);
    if (runtime_log_store.magic == kRuntimeLogMagic &&
        runtime_log_store.write_index < kRuntimeLogCapacity) {
        for (size_t i = 0; i < length; ++i) {
            runtime_log_store.data[runtime_log_store.write_index] = data[i];
            runtime_log_store.write_index = (runtime_log_store.write_index + 1) % kRuntimeLogCapacity;
            ++runtime_log_store.total_bytes;
        }
    }
    portEXIT_CRITICAL(&runtime_log_mux);
}

int CaptureRuntimeLogVprintf(const char* format, va_list args) {
    char buffer[256];
    va_list copy;
    va_copy(copy, args);
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, copy);
    va_end(copy);
    if (length > 0) {
        AppendRuntimeLog(buffer, std::min(static_cast<size_t>(length), sizeof(buffer) - 1));
    }
    return original_log_vprintf ? original_log_vprintf(format, args) : length;
}

void StartRuntimeLogCaptureInternal() {
    if (runtime_log_capture_started) return;
    EnsureRuntimeLogStore();
    original_log_vprintf = esp_log_set_vprintf(CaptureRuntimeLogVprintf);
    runtime_log_capture_started = true;
    char marker[96];
    const int length = std::snprintf(marker, sizeof(marker),
        "\n--- ChatAI boot; previous reset: %s ---\n", ResetReasonLabel(esp_reset_reason()));
    if (length > 0) AppendRuntimeLog(marker, std::min(static_cast<size_t>(length), sizeof(marker) - 1));
}

size_t CopyRuntimeLogSnapshot(char* output, size_t capacity) {
    if (output == nullptr || capacity == 0) return 0;
    portENTER_CRITICAL(&runtime_log_mux);
    if (runtime_log_store.magic != kRuntimeLogMagic ||
        runtime_log_store.write_index >= kRuntimeLogCapacity) {
        output[0] = '\0';
        portEXIT_CRITICAL(&runtime_log_mux);
        return 0;
    }
    const size_t count = std::min(
        std::min(static_cast<size_t>(runtime_log_store.total_bytes), kRuntimeLogCapacity), capacity - 1);
    const size_t start = (runtime_log_store.write_index + kRuntimeLogCapacity - count) % kRuntimeLogCapacity;
    for (size_t i = 0; i < count; ++i) {
        output[i] = runtime_log_store.data[(start + i) % kRuntimeLogCapacity];
    }
    output[count] = '\0';
    portEXIT_CRITICAL(&runtime_log_mux);
    return count;
}

void ClearRuntimeLog() {
    portENTER_CRITICAL(&runtime_log_mux);
    if (runtime_log_store.magic == kRuntimeLogMagic) {
        runtime_log_store.write_index = 0;
        runtime_log_store.total_bytes = 0;
        std::memset(runtime_log_store.data, 0, sizeof(runtime_log_store.data));
    }
    portEXIT_CRITICAL(&runtime_log_mux);
    static constexpr char marker[] = "--- runtime log cleared from Web Admin ---\n";
    AppendRuntimeLog(marker, sizeof(marker) - 1);
}

const char* ResetReasonLabel(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "POWER ON";
        case ESP_RST_EXT: return "EXTERNAL";
        case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT WDT";
        case ESP_RST_TASK_WDT: return "TASK WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "UNKNOWN";
    }
}

void SendJson(httpd_req_t* req, cJSON* root, const char* status = "200 OK") {
    char* encoded = cJSON_PrintUnformatted(root);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, encoded ? encoded : "{}");
    if (encoded) cJSON_free(encoded);
    cJSON_Delete(root);
}

void SendError(httpd_req_t* req, const char* status, const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message);
    SendJson(req, root, status);
}

bool IsValidWakeWord(const char* value) {
    if (value == nullptr) return false;
    const std::string phrase(value);
    if (phrase.size() < 3 || phrase.size() > 32 || phrase.front() == ' ' || phrase.back() == ' ') return false;
    bool previous_space = false;
    for (unsigned char c : phrase) {
        if (c == ' ') {
            if (previous_space) return false;
            previous_space = true;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            previous_space = false;
        } else {
            return false;
        }
    }
    return true;
}

bool IsAllowedVoice(const char* value) {
    static const char* voices[] = {
        "Zephyr", "Kore", "Orus", "Autonoe", "Umbriel", "Erinome", "Laomedeia",
        "Schedar", "Achird", "Sadachbia", "Puck", "Fenrir", "Aoede", "Enceladus",
        "Algieba", "Algenib", "Achernar", "Gacrux", "Zubenelgenubi", "Sadaltager",
        "Charon", "Leda", "Callirrhoe", "Iapetus", "Despina", "Rasalgethi",
        "Alnilam", "Pulcherrima", "Vindemiatrix", "Sulafat"
    };
    if (value == nullptr) return false;
    for (const char* voice : voices) if (std::strcmp(value, voice) == 0) return true;
    return false;
}

bool IsAllowedLanguage(const char* value) {
    static const char* languages[] = {
        "ar-EG", "bn-BD", "nl-NL", "en-IN", "en-US", "fr-FR", "de-DE", "hi-IN",
        "id-ID", "it-IT", "ja-JP", "ko-KR", "mr-IN", "pl-PL", "pt-BR", "ro-RO",
        "ru-RU", "es-US", "ta-IN", "te-IN", "th-TH", "tr-TR", "uk-UA", "vi-VN"
    };
    if (value == nullptr) return false;
    for (const char* language : languages) if (std::strcmp(value, language) == 0) return true;
    return false;
}

esp_err_t IndexHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, index_html_start);
}

esp_err_t StatusHandler(httpd_req_t* req) {
    auto& wifi = WifiManager::GetInstance();
    Settings gemini("gemini", false);
    Settings audio("chatai", false);
    const auto* app = esp_app_get_description();
    const auto* running = esp_ota_get_running_partition();
    const auto* target = esp_ota_get_next_update_partition(nullptr);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ip", wifi.GetIpAddress().c_str());
    cJSON_AddStringToObject(root, "hostname", "chatai-tpios.local");
    cJSON_AddStringToObject(root, "ssid", wifi.GetSsid().c_str());
    cJSON_AddBoolToObject(root, "enabled", gemini.GetBool("enabled", false));
    cJSON_AddBoolToObject(root, "key_configured", !gemini.GetString("api_key").empty());
    cJSON_AddStringToObject(root, "version", app ? app->version : "unknown");
    cJSON_AddStringToObject(root, "partition", running ? running->label : "unknown");
    cJSON_AddStringToObject(root, "ota_target", target ? target->label : "unavailable");
    cJSON_AddNumberToObject(root, "ota_max_bytes", target ? target->size : 0);
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    cJSON_AddBoolToObject(root, "ota_enabled", true);
#else
    cJSON_AddBoolToObject(root, "ota_enabled", false);
#endif
    cJSON_AddNumberToObject(root, "volume", audio.GetInt("volume", 70));
    const auto reset_reason = esp_reset_reason();
    cJSON_AddStringToObject(root, "reset_reason", ResetReasonLabel(reset_reason));
    cJSON_AddBoolToObject(root, "panic_detected", reset_reason == ESP_RST_PANIC);
    const auto diagnostics = GeminiLiveProtocol::GetDiagnostics();
    cJSON_AddNumberToObject(root, "mic_chunks_sent", diagnostics.microphone_chunks_sent);
    cJSON_AddNumberToObject(root, "response_audio_chunks", diagnostics.response_audio_chunks_received);
    cJSON_AddBoolToObject(root, "live_session_connected", diagnostics.session_connected);
    cJSON_AddBoolToObject(root, "mic_streaming", diagnostics.microphone_streaming);
    cJSON_AddStringToObject(root, "live_last_error", diagnostics.last_error.c_str());
    cJSON_AddStringToObject(root, "live_event_history", diagnostics.event_history.c_str());
    cJSON_AddStringToObject(root, "live_previous_session", diagnostics.previous_session_summary.c_str());
    SendJson(req, root);
    return ESP_OK;
}

esp_err_t ResetReportHandler(httpd_req_t* req) {
    const auto reason = esp_reset_reason();
    const auto* app = esp_app_get_description();
    const auto* running = esp_ota_get_running_partition();
    char report[512] = {};
    const int length = std::snprintf(
        report, sizeof(report),
        "ChatAI TPIOS - Reset report\n"
        "Firmware: %s\n"
        "Running partition: %s\n"
        "Reset reason: %s\n"
        "PANIC detected: %s\n\n"
        "Limit: this firmware has no Core Dump partition configured. "
        "This report contains the reset reason only; it does not contain "
        "CPU registers, PC/LR, task stack or backtrace.\n",
        app ? app->version : "unknown",
        running ? running->label : "unknown",
        ResetReasonLabel(reason),
        reason == ESP_RST_PANIC ? "yes" : "no");
    if (length < 0) {
        SendError(req, "500 Internal Server Error", "Could not create reset report");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=chatai-reset-report.txt");
    return httpd_resp_send(req, report, std::min(static_cast<size_t>(length), sizeof(report) - 1));
}

esp_err_t ConfigGetHandler(httpd_req_t* req) {
    Settings gemini("gemini", false);
    Settings audio("chatai", false);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", gemini.GetString("model", "gemini-3.8-live").c_str());
    cJSON_AddStringToObject(root, "system_prompt", gemini.GetString(
        "system_prompt",
        "A helpful Vietnamese voice assistant. Speak naturally in Vietnamese unless the user asks for another language. Keep replies concise and conversational."
    ).c_str());
    cJSON_AddBoolToObject(root, "enabled", gemini.GetBool("enabled", false));
    cJSON_AddStringToObject(root, "bot_name", gemini.GetString("bot_name", "ChatAI").c_str());
    cJSON_AddStringToObject(root, "voice_name", gemini.GetString("voice_name", "Puck").c_str());
    cJSON_AddStringToObject(root, "language_code", gemini.GetString("language_code", "vi-VN").c_str());
    cJSON_AddStringToObject(root, "wake_word", audio.GetString("wake_word", "Hey ChatAI").c_str());
    cJSON_AddNumberToObject(root, "mic_gain", audio.GetInt("mic_gain", 0));
    cJSON_AddNumberToObject(root, "volume", audio.GetInt("volume", 70));
    SendJson(req, root);
    return ESP_OK;
}

bool IsSameOrigin(httpd_req_t* req) {
    const size_t origin_length = httpd_req_get_hdr_value_len(req, "Origin");
    if (origin_length == 0) return true;
    const size_t host_length = httpd_req_get_hdr_value_len(req, "Host");
    if (origin_length >= 128 || host_length == 0 || host_length >= 128) return false;

    char origin[128] = {};
    char host[128] = {};
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }

    constexpr char kHttpPrefix[] = "http://";
    if (std::strncmp(origin, kHttpPrefix, sizeof(kHttpPrefix) - 1) != 0) return false;

    std::string request_host(host);
    if (request_host.size() > 3 && request_host.compare(request_host.size() - 3, 3, ":80") == 0) {
        request_host.resize(request_host.size() - 3);
    }
    const bool allowed_host =
        request_host == WifiManager::GetInstance().GetIpAddress() ||
        request_host == "chatai-tpios.local";
    return allowed_host && origin == std::string(kHttpPrefix) + request_host;
}

esp_err_t RuntimeLogHandler(httpd_req_t* req) {
    auto* runtime_log = static_cast<char*>(std::malloc(kRuntimeLogCapacity + 1));
    if (runtime_log == nullptr) {
        SendError(req, "503 Service Unavailable", "Not enough memory to read diagnostics");
        return ESP_OK;
    }
    const size_t runtime_length = CopyRuntimeLogSnapshot(runtime_log, kRuntimeLogCapacity + 1);
    const auto* app = esp_app_get_description();
    const auto* running = esp_ota_get_running_partition();
    const auto diagnostics = GeminiLiveProtocol::GetDiagnostics();
    char header[512] = {};
    std::snprintf(header, sizeof(header),
        "ChatAI TPIOS - COMBINED DIAGNOSTICS\n"
        "Firmware: %s\n"
        "Running partition: %s\n"
        "Reset reason: %s\n"
        "PANIC detected: %s\n\n"
        "GEMINI LIVE STATUS\n"
        "Session connected: %s\n"
        "Microphone streaming: %s\n"
        "Microphone chunks sent: %lu\n"
        "Response audio chunks: %lu\n"
        "Last Gemini error: %s\n\n",
        app ? app->version : "unknown",
        running ? running->label : "unknown",
        ResetReasonLabel(esp_reset_reason()),
        esp_reset_reason() == ESP_RST_PANIC ? "YES" : "NO",
        diagnostics.session_connected ? "YES" : "NO",
        diagnostics.microphone_streaming ? "YES" : "NO",
        static_cast<unsigned long>(diagnostics.microphone_chunks_sent),
        static_cast<unsigned long>(diagnostics.response_audio_chunks_received),
        diagnostics.last_error.empty() ? "None" : diagnostics.last_error.c_str());

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=chatai-diagnostics.txt");
    esp_err_t result = httpd_resp_sendstr_chunk(req, header);
    if (result == ESP_OK) result = httpd_resp_sendstr_chunk(req, "PREVIOUS GEMINI SESSION\n");
    if (result == ESP_OK) result = httpd_resp_sendstr_chunk(req,
        diagnostics.previous_session_summary.empty()
            ? "No previous Gemini session snapshot.\n"
            : diagnostics.previous_session_summary.c_str());
    if (result == ESP_OK) result = httpd_resp_sendstr_chunk(req, "\nGEMINI EVENT HISTORY\n");
    if (result == ESP_OK) result = httpd_resp_sendstr_chunk(req,
        diagnostics.event_history.empty()
            ? "No retained Gemini events.\n"
            : diagnostics.event_history.c_str());
    if (result == ESP_OK) result = httpd_resp_sendstr_chunk(req, "\nSYSTEM RUNTIME LOG (latest 4 KB)\n");
    if (result == ESP_OK && runtime_length > 0) {
        result = httpd_resp_send_chunk(req, runtime_log, runtime_length);
    }
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, nullptr, 0);
    std::free(runtime_log);
    return result;
}

esp_err_t ClearRuntimeLogHandler(httpd_req_t* req) {
    if (!IsSameOrigin(req)) {
        SendError(req, "403 Forbidden", "Cross-origin request rejected");
        return ESP_OK;
    }
    ClearRuntimeLog();
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "message", "Runtime log cleared.");
    SendJson(req, response);
    return ESP_OK;
}

esp_err_t ConfigPostHandler(httpd_req_t* req) {
    if (!IsSameOrigin(req)) {
        SendError(req, "403 Forbidden", "Cross-origin request rejected");
        return ESP_OK;
    }

    auto display = Board::GetInstance().GetDisplay();
    if (display) display->SetStatus("Đang lưu cấu hình...");
    auto fail = [&](const char* status, const char* message) {
        if (display) display->SetStatus("Lưu cấu hình thất bại");
        Application::GetInstance().SpeakLiveNotification(
            "Lưu cấu hình thất bại, vui lòng kiểm tra lại.");
        SendError(req, status, message);
        return ESP_OK;
    };

    if (req->content_len == 0 || req->content_len > kMaxBody) {
        return fail("400 Bad Request", "Invalid configuration size");
    }

    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int count = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (count <= 0) {
            return fail("400 Bad Request", "Incomplete request body");
        }
        received += static_cast<size_t>(count);
    }

    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return fail("400 Bad Request", "Invalid JSON");
    }

    const cJSON* key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    const cJSON* model = cJSON_GetObjectItemCaseSensitive(root, "model");
    const cJSON* prompt = cJSON_GetObjectItemCaseSensitive(root, "system_prompt");
    const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON* volume = cJSON_GetObjectItemCaseSensitive(root, "volume");
    const cJSON* bot_name = cJSON_GetObjectItemCaseSensitive(root, "bot_name");
    const cJSON* voice_name = cJSON_GetObjectItemCaseSensitive(root, "voice_name");
    const cJSON* language_code = cJSON_GetObjectItemCaseSensitive(root, "language_code");
    const cJSON* wake_word = cJSON_GetObjectItemCaseSensitive(root, "wake_word");
    const cJSON* mic_gain = cJSON_GetObjectItemCaseSensitive(root, "mic_gain");
    const bool has_wake_word = wake_word != nullptr;

    const bool valid =
        (!key || (cJSON_IsString(key) && std::string(key->valuestring).size() <= 256)) &&
        cJSON_IsString(model) && std::strlen(model->valuestring) > 0 &&
        std::strlen(model->valuestring) <= 96 &&
        cJSON_IsString(prompt) && std::strlen(prompt->valuestring) <= kMaxSystemPromptBytes &&
        cJSON_IsBool(enabled) &&
        (!volume || (cJSON_IsNumber(volume) && volume->valueint >= 0 && volume->valueint <= 100)) &&
        (!bot_name || (cJSON_IsString(bot_name) && std::strlen(bot_name->valuestring) > 0 && std::strlen(bot_name->valuestring) <= 48)) &&
        (!voice_name || (cJSON_IsString(voice_name) && IsAllowedVoice(voice_name->valuestring))) &&
        (!language_code || (cJSON_IsString(language_code) && IsAllowedLanguage(language_code->valuestring))) &&
        (!wake_word || (cJSON_IsString(wake_word) && IsValidWakeWord(wake_word->valuestring))) &&
        (!mic_gain || (cJSON_IsNumber(mic_gain) && mic_gain->valueint >= 0 && mic_gain->valueint <= 20));
    if (!valid) {
        cJSON_Delete(root);
        return fail("400 Bad Request",
                    "Invalid model, prompt, key, voice, language, wake phrase, mic gain, volume or enabled value");
    }

    Settings existing("gemini", false);
    const std::string expected_api_key =
        key && key->valuestring && key->valuestring[0] != '\0'
            ? key->valuestring : existing.GetString("api_key");
    const bool has_key = !expected_api_key.empty();
    if (cJSON_IsTrue(enabled) && !has_key) {
        cJSON_Delete(root);
        return fail("400 Bad Request", "Enter an API key before enabling Gemini Live");
    }

    {
        Settings settings("gemini", true);
        if (key && key->valuestring && key->valuestring[0] != '\0') {
            settings.SetString("api_key", key->valuestring);
        }
        settings.SetString("model", model->valuestring);
        settings.SetString("system_prompt", prompt->valuestring);
        settings.SetBool("enabled", cJSON_IsTrue(enabled));
        if (bot_name) settings.SetString("bot_name", bot_name->valuestring);
        if (voice_name) settings.SetString("voice_name", voice_name->valuestring);
        if (language_code) settings.SetString("language_code", language_code->valuestring);
    }

    if (wake_word) {
        Settings settings("chatai", true);
        settings.SetString("wake_word", wake_word->valuestring);
    }
    if (mic_gain) {
        Settings settings("chatai", true);
        settings.SetInt("mic_gain", mic_gain->valueint);
        Board::GetInstance().GetAudioCodec()->SetInputGain(static_cast<float>(mic_gain->valueint));
    }
    if (volume) {
        Settings settings("chatai", true);
        settings.SetInt("volume", volume->valueint);
        Board::GetInstance().GetAudioCodec()->SetOutputVolume(volume->valueint);
    }

    // Settings commits NVS when each write handle is closed. Read every supplied
    // value back before reporting success; never include the API key in a response.
    bool persisted = true;
    const std::string missing(kMaxSystemPromptBytes + 1, '\x1f');
    {
        Settings saved("gemini", false);
        persisted =
            saved.GetString("model", missing) == model->valuestring &&
            saved.GetString("system_prompt", missing) == prompt->valuestring &&
            saved.GetBool("enabled", !cJSON_IsTrue(enabled)) == cJSON_IsTrue(enabled);
        if (!expected_api_key.empty()) {
            persisted = persisted && saved.GetString("api_key", missing) == expected_api_key;
        }
        if (bot_name) persisted = persisted && saved.GetString("bot_name", missing) == bot_name->valuestring;
        if (voice_name) persisted = persisted && saved.GetString("voice_name", missing) == voice_name->valuestring;
        if (language_code) persisted = persisted && saved.GetString("language_code", missing) == language_code->valuestring;
    }
    {
        Settings saved("chatai", false);
        if (wake_word) persisted = persisted && saved.GetString("wake_word", missing) == wake_word->valuestring;
        if (mic_gain) persisted = persisted && saved.GetInt("mic_gain", -1) == mic_gain->valueint;
        if (volume) persisted = persisted && saved.GetInt("volume", -1) == volume->valueint;
    }
    if (!persisted) {
        cJSON_Delete(root);
        return fail("500 Internal Server Error",
                    "Không xác minh được cấu hình đã lưu trong NVS. Vui lòng thử lại.");
    }

    bool wake_word_applied = false;
    if (wake_word) {
        wake_word_applied =
            Application::GetInstance().GetAudioService().SetWakeWordPhrase(wake_word->valuestring);
    }
    cJSON_Delete(root);

    if (display) display->SetStatus("Lưu cấu hình thành công");
    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "wake_word_applied", wake_word_applied);
    cJSON_AddStringToObject(response, "message", has_wake_word
        ? (wake_word_applied
            ? "Đã lưu cấu hình; Wake Word đã được cập nhật. Gemini Live áp dụng cấu hình mới ở phiên kết nối tiếp theo."
            : "Đã lưu cấu hình. Wake Word sẽ được nạp khi khởi động lại; Gemini Live áp dụng cấu hình mới ở phiên kết nối tiếp theo.")
        : "Đã lưu cấu hình. Gemini Live áp dụng cấu hình mới ở phiên kết nối tiếp theo.");
    SendJson(req, response);
    Application::GetInstance().SpeakLiveNotification("Đã lưu cấu hình thành công.");
    return ESP_OK;
}

esp_err_t WifiConfigHandler(httpd_req_t* req) {
    if (!IsSameOrigin(req)) {
        SendError(req, "403 Forbidden", "Cross-origin request rejected");
        return ESP_OK;
    }
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "message", "Đang chuyển sang chế độ cấu hình Wi-Fi. Kết nối điểm truy cập ChatAI-TPIOS rồi mở 192.168.4.1.");
    SendJson(req, response);
    vTaskDelay(pdMS_TO_TICKS(250));
    static_cast<WifiBoard&>(Board::GetInstance()).EnterWifiConfigMode();
    return ESP_OK;
}

esp_err_t RebootHandler(httpd_req_t* req) {
    if (!IsSameOrigin(req)) {
        SendError(req, "403 Forbidden", "Cross-origin request rejected");
        return ESP_OK;
    }
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "message", "Thiết bị đang khởi động lại.");
    SendJson(req, response);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t FactoryResetHandler(httpd_req_t* req) {
    if (!IsSameOrigin(req)) {
        SendError(req, "403 Forbidden", "Cross-origin request rejected");
        return ESP_OK;
    }
    const esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        SendError(req, "500 Internal Server Error", "Không thể xóa cấu hình NVS.");
        return ESP_OK;
    }
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "message", "Đã xóa cấu hình lưu trên thiết bị. Thiết bị đang khởi động lại vào chế độ cấu hình Wi-Fi.");
    SendJson(req, response);
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_restart();
    return ESP_OK;
}

esp_err_t OtaUploadHandler(httpd_req_t* req) {
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    SendError(req, "503 Service Unavailable", "OTA rollback is disabled in this firmware build");
    return ESP_OK;
#else
    if (!IsSameOrigin(req)) {
        SendError(req, "403 Forbidden", "Cross-origin request rejected");
        return ESP_OK;
    }
    if (req->content_len < 4096) {
        SendError(req, "400 Bad Request", "Firmware file is empty or too small");
        return ESP_OK;
    }

    std::unique_lock<std::mutex> ota_lock(ota_mutex, std::try_to_lock);
    if (!ota_lock.owns_lock()) {
        SendError(req, "409 Conflict", "Another firmware update is already running");
        return ESP_OK;
    }

    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus("OTA kiem tra");

    const auto* ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
    const auto* ota1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1");
    const auto* otadata = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata");
    const auto* running = esp_ota_get_running_partition();
    const auto* target = esp_ota_get_next_update_partition(nullptr);

    if (!ota0 || !ota1 || !otadata || !running || !target ||
        target == running || target->type != ESP_PARTITION_TYPE_APP ||
        (target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) {
        display->SetStatus("OTA loi phan vung");
        SendError(req, "503 Service Unavailable", "A valid dual-slot OTA partition layout was not found");
        return ESP_OK;
    }
    if (static_cast<size_t>(req->content_len) > target->size) {
        display->SetStatus("OTA file qua lon");
        SendError(req, "413 Content Too Large", "Firmware image exceeds the inactive OTA slot");
        return ESP_OK;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
    if (err != ESP_OK) {
        display->SetStatus("OTA loi bat dau");
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        SendError(req, "500 Internal Server Error", "Could not start writing the OTA image");
        return ESP_OK;
    }

    display->SetStatus("OTA dang tai 0%");
    uint8_t buffer[kOtaChunkSize];
    size_t remaining = static_cast<size_t>(req->content_len);
    size_t written = 0;
    int last_display_progress = -10;
    while (remaining > 0) {
        const size_t wanted = std::min(remaining, sizeof(buffer));
        const int received = httpd_req_recv(req, reinterpret_cast<char*>(buffer), wanted);
        if (received <= 0) {
            esp_ota_abort(handle);
            display->SetStatus("OTA loi tai");
            ESP_LOGE(TAG, "OTA upload interrupted after %u bytes", static_cast<unsigned>(written));
            SendError(req, "400 Bad Request", "Firmware upload was interrupted");
            return ESP_OK;
        }
        err = esp_ota_write(handle, buffer, static_cast<size_t>(received));
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            display->SetStatus("OTA loi ghi");
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            SendError(req, "500 Internal Server Error", "Could not write firmware to the inactive slot");
            return ESP_OK;
        }
        remaining -= static_cast<size_t>(received);
        written += static_cast<size_t>(received);
        const int progress = static_cast<int>(written * 100 / static_cast<size_t>(req->content_len));
        if (progress >= last_display_progress + 10) {
            char status[20];
            std::snprintf(status, sizeof(status), "OTA %d%%", progress);
            display->SetStatus(status);
            last_display_progress = progress;
        }
    }

    display->SetStatus("OTA kiem tra file");
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        display->SetStatus("OTA file khong hop le");
        ESP_LOGE(TAG, "OTA image validation failed: %s", esp_err_to_name(err));
        SendError(req, "400 Bad Request", "Firmware image is invalid or incompatible");
        return ESP_OK;
    }
    display->SetStatus("OTA chuan bi cai");
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        display->SetStatus("OTA loi chon slot");
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        SendError(req, "500 Internal Server Error", "Could not select the new firmware for boot");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA image accepted: %u bytes -> %s", static_cast<unsigned>(written), target->label);
    display->SetStatus("OTA khoi dong");
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "message", "Firmware đã được xác thực và cài đặt. Thiết bị đang khởi động lại.");
    SendJson(req, response);
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;
#endif
}

} // namespace

void WebAdminServer::InitializeDiagnostics() {
    GeminiLiveProtocol::InitializeDiagnosticHistory();
    StartRuntimeLogCaptureInternal();
}

void WebAdminServer::Start() {
    std::lock_guard<std::mutex> lock(server_mutex);
    if (server) return;

    StartRuntimeLogCaptureInternal();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 11;
    config.stack_size = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        server = nullptr;
        ESP_LOGE(TAG, "Could not start local Web Admin");
        return;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = IndexHandler, .user_ctx = nullptr},
        {.uri = "/api/status", .method = HTTP_GET, .handler = StatusHandler, .user_ctx = nullptr},
        {.uri = "/api/reset-report", .method = HTTP_GET, .handler = ResetReportHandler, .user_ctx = nullptr},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = RuntimeLogHandler, .user_ctx = nullptr},
        {.uri = "/api/logs/clear", .method = HTTP_POST, .handler = ClearRuntimeLogHandler, .user_ctx = nullptr},
        {.uri = "/api/config", .method = HTTP_GET, .handler = ConfigGetHandler, .user_ctx = nullptr},
        {.uri = "/api/config", .method = HTTP_POST, .handler = ConfigPostHandler, .user_ctx = nullptr},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = OtaUploadHandler, .user_ctx = nullptr},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = RebootHandler, .user_ctx = nullptr},
        {.uri = "/api/wifi-config", .method = HTTP_POST, .handler = WifiConfigHandler, .user_ctx = nullptr},
        {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = FactoryResetHandler, .user_ctx = nullptr},
    };
    bool ok = true;
    for (const auto& route : routes) {
        if (httpd_register_uri_handler(server, &route) != ESP_OK) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        httpd_stop(server);
        server = nullptr;
        ESP_LOGE(TAG, "Could not register local Web Admin routes");
        return;
    }
    ESP_LOGI(TAG, "Local Web Admin started on port 80");
}

void WebAdminServer::Stop() {
    std::lock_guard<std::mutex> lock(server_mutex);
    if (!server) return;
    httpd_stop(server);
    server = nullptr;
    ESP_LOGI(TAG, "Local Web Admin stopped");
}
