#include "web_admin_server.h"

#include "board.h"
#include "audio_codec.h"
#include "application.h"
#include "wifi_board.h"
#include "settings.h"
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

extern const char index_html_start[] asm("_binary_wifi_configuration_html_start");

namespace {
constexpr char TAG[] = "WebAdmin";
constexpr size_t kMaxBody = 4096;
constexpr size_t kOtaChunkSize = 1024;
httpd_handle_t server = nullptr;
std::mutex server_mutex;
std::mutex ota_mutex;

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
    SendJson(req, root);
    return ESP_OK;
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
    const size_t length = httpd_req_get_hdr_value_len(req, "Origin");
    if (length == 0) return true;
    if (length >= 128) return false;
    char origin[128] = {};
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) return false;
    const std::string expected = "http://" + WifiManager::GetInstance().GetIpAddress();
    return expected == origin;
}

esp_err_t ConfigPostHandler(httpd_req_t* req) {
    if (!IsSameOrigin(req)) {
        SendError(req, "403 Forbidden", "Cross-origin request rejected");
        return ESP_OK;
    }
    if (req->content_len == 0 || req->content_len > kMaxBody) {
        SendError(req, "400 Bad Request", "Invalid configuration size");
        return ESP_OK;
    }

    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int count = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (count <= 0) {
            SendError(req, "400 Bad Request", "Incomplete request body");
            return ESP_OK;
        }
        received += static_cast<size_t>(count);
    }

    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        SendError(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
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
        cJSON_IsString(prompt) && std::strlen(prompt->valuestring) <= 1024 &&
        cJSON_IsBool(enabled) &&
        (!volume || (cJSON_IsNumber(volume) && volume->valueint >= 0 && volume->valueint <= 100)) &&
        (!bot_name || (cJSON_IsString(bot_name) && std::strlen(bot_name->valuestring) > 0 && std::strlen(bot_name->valuestring) <= 48)) &&
        (!voice_name || (cJSON_IsString(voice_name) && IsAllowedVoice(voice_name->valuestring))) &&
        (!language_code || (cJSON_IsString(language_code) && IsAllowedLanguage(language_code->valuestring))) &&
        (!wake_word || (cJSON_IsString(wake_word) && IsValidWakeWord(wake_word->valuestring))) &&
        (!mic_gain || (cJSON_IsNumber(mic_gain) && mic_gain->valueint >= 0 && mic_gain->valueint <= 20));
    if (!valid) {
        cJSON_Delete(root);
        SendError(req, "400 Bad Request", "Invalid model, prompt, key, voice, language, wake phrase, mic gain, volume or enabled value");
        return ESP_OK;
    }

    Settings existing("gemini", false);
    const bool has_key = !existing.GetString("api_key").empty() ||
                         (key && key->valuestring && key->valuestring[0] != '\0');
    if (cJSON_IsTrue(enabled) && !has_key) {
        cJSON_Delete(root);
        SendError(req, "400 Bad Request", "Enter an API key before enabling Gemini Live");
        return ESP_OK;
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
    bool wake_word_applied = false;
    if (wake_word) {
        Settings settings("chatai", true);
        settings.SetString("wake_word", wake_word->valuestring);
    }
    if (mic_gain) {
        Board::GetInstance().GetAudioCodec()->SetInputGain(static_cast<float>(mic_gain->valueint));
    }
    if (wake_word) {
        wake_word_applied = Application::GetInstance().GetAudioService().SetWakeWordPhrase(wake_word->valuestring);
    }
    if (volume) {
        Settings settings("chatai", true);
        settings.SetInt("volume", volume->valueint);
        Board::GetInstance().GetAudioCodec()->SetOutputVolume(volume->valueint);
    }
    cJSON_Delete(root);

    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "wake_word_applied", wake_word_applied);
    cJSON_AddStringToObject(response, "message", has_wake_word
        ? (wake_word_applied
            ? "Đã lưu cấu hình; Wake Word đã được cập nhật. Gemini Live áp dụng cấu hình mới ở phiên kết nối tiếp theo."
            : "Đã lưu cấu hình. Wake Word sẽ được nạp khi khởi động lại; Gemini Live áp dụng cấu hình mới ở phiên kết nối tiếp theo.")
        : "Đã lưu cấu hình. Gemini Live áp dụng cấu hình mới ở phiên kết nối tiếp theo.");
    SendJson(req, response);
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

    const auto* ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
    const auto* ota1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1");
    const auto* otadata = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata");
    const auto* running = esp_ota_get_running_partition();
    const auto* target = esp_ota_get_next_update_partition(nullptr);

    if (!ota0 || !ota1 || !otadata || !running || !target ||
        target == running || target->type != ESP_PARTITION_TYPE_APP ||
        (target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) {
        SendError(req, "503 Service Unavailable", "A valid dual-slot OTA partition layout was not found");
        return ESP_OK;
    }
    if (static_cast<size_t>(req->content_len) > target->size) {
        SendError(req, "413 Content Too Large", "Firmware image exceeds the inactive OTA slot");
        return ESP_OK;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        SendError(req, "500 Internal Server Error", "Could not start writing the OTA image");
        return ESP_OK;
    }

    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus("OTA 0%");
    uint8_t buffer[kOtaChunkSize];
    size_t remaining = static_cast<size_t>(req->content_len);
    size_t written = 0;
    int last_display_progress = -10;
    while (remaining > 0) {
        const size_t wanted = std::min(remaining, sizeof(buffer));
        const int received = httpd_req_recv(req, reinterpret_cast<char*>(buffer), wanted);
        if (received <= 0) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "OTA upload interrupted after %u bytes", static_cast<unsigned>(written));
            SendError(req, "400 Bad Request", "Firmware upload was interrupted");
            return ESP_OK;
        }
        err = esp_ota_write(handle, buffer, static_cast<size_t>(received));
        if (err != ESP_OK) {
            esp_ota_abort(handle);
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

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA image validation failed: %s", esp_err_to_name(err));
        SendError(req, "400 Bad Request", "Firmware image is invalid or incompatible");
        return ESP_OK;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        SendError(req, "500 Internal Server Error", "Could not select the new firmware for boot");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA image accepted: %u bytes -> %s", static_cast<unsigned>(written), target->label);
    display->SetStatus("OTA hoan tat");
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "message", "Firmware đã được xác thực và cài đặt. Thiết bị đang khởi động lại.");
    SendJson(req, response);
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;
#endif
}

} // namespace

void WebAdminServer::Start() {
    std::lock_guard<std::mutex> lock(server_mutex);
    if (server) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    config.stack_size = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        server = nullptr;
        ESP_LOGE(TAG, "Could not start local Web Admin");
        return;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = IndexHandler, .user_ctx = nullptr},
        {.uri = "/api/status", .method = HTTP_GET, .handler = StatusHandler, .user_ctx = nullptr},
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
