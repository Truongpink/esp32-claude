#include "gemini_settings.h"

#include <nvs.h>
#include <algorithm>
#include <cstring>
#include <esp_log.h>

namespace {
constexpr char kNvsNamespace[] = "chatai_ai";
constexpr char kApiKey[] = "gemini_key";
constexpr char kVoice[] = "voice";
constexpr char kSystemPrompt[] = "system_prompt";
constexpr size_t kMaxSystemPromptBytes = 3000;
constexpr const char* kDefaultVoice = "Kore";
constexpr const char* kDefaultSystemPrompt = "Bạn là trợ lý ChatAI TPIOS. Hãy giao tiếp tự nhiên, thân thiện và rõ ràng bằng tiếng Việt. Chỉ trả lời bằng giọng nói trong phiên hội thoại thời gian thực.";
constexpr size_t kMaxApiKeyLength = 256;
}

namespace chatai::settings {

esp_err_t MigrateLegacyGeminiApiKey() {
    if (HasGeminiApiKey()) return ESP_OK;

    nvs_handle_t legacy_handle;
    if (nvs_open("gemini", NVS_READONLY, &legacy_handle) != ESP_OK) {
        return ESP_OK;
    }

    size_t size = 0;
    esp_err_t result = nvs_get_str(legacy_handle, "api_key", nullptr, &size);
    if (result != ESP_OK || size <= 1 || size > kMaxApiKeyLength + 1) {
        nvs_close(legacy_handle);
        return ESP_OK;
    }

    std::string legacy_key(size, '\0');
    result = nvs_get_str(legacy_handle, "api_key", legacy_key.data(), &size);
    nvs_close(legacy_handle);
    if (result != ESP_OK) return ESP_OK;

    legacy_key.resize(size - 1);
    result = SaveGeminiApiKey(legacy_key);
    std::fill(legacy_key.begin(), legacy_key.end(), '\0');
    if (result == ESP_OK) {
        ESP_LOGI("ChatAI-Settings", "Migrated existing Gemini API Key into standalone settings");
    }
    return result;
}


bool IsSupportedGeminiVoice(const std::string& voice) {
    static constexpr const char* kVoices[] = {
        "Achernar", "Achird", "Algenib", "Algieba", "Alnilam", "Aoede",
        "Autonoe", "Callirrhoe", "Charon", "Despina", "Enceladus", "Erinome",
        "Fenrir", "Gacrux", "Iapetus", "Kore", "Laomedeia", "Leda", "Orus",
        "Puck", "Pulcherrima", "Rasalgethi", "Sadachbia", "Sadaltager",
        "Schedar", "Sulafat", "Umbriel", "Vindemiatrix", "Zephyr", "Zubenelgenubi"
    };
    for (const char* candidate : kVoices) {
        if (voice == candidate) return true;
    }
    return false;
}

std::string GetGeminiVoice() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return kDefaultVoice;
    size_t size = 0;
    if (nvs_get_str(handle, kVoice, nullptr, &size) != ESP_OK || size <= 1 || size > 32) {
        nvs_close(handle);
        return kDefaultVoice;
    }
    std::string value(size, '\\0');
    const esp_err_t result = nvs_get_str(handle, kVoice, value.data(), &size);
    nvs_close(handle);
    if (result != ESP_OK) return kDefaultVoice;
    value.resize(size - 1);
    return IsSupportedGeminiVoice(value) ? value : kDefaultVoice;
}

esp_err_t SaveGeminiVoice(const std::string& voice) {
    if (!IsSupportedGeminiVoice(voice)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_str(handle, kVoice, voice.c_str());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

std::string GetGeminiSystemPrompt() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return kDefaultSystemPrompt;
    size_t size = 0;
    if (nvs_get_str(handle, kSystemPrompt, nullptr, &size) != ESP_OK ||
        size <= 1 || size > kMaxSystemPromptBytes + 1) {
        nvs_close(handle);
        return kDefaultSystemPrompt;
    }
    std::string value(size, '\\0');
    const esp_err_t result = nvs_get_str(handle, kSystemPrompt, value.data(), &size);
    nvs_close(handle);
    if (result != ESP_OK) return kDefaultSystemPrompt;
    value.resize(size - 1);
    return value;
}

esp_err_t SaveGeminiSystemPrompt(const std::string& prompt) {
    if (prompt.empty() || prompt.size() > kMaxSystemPromptBytes) return ESP_ERR_INVALID_SIZE;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_str(handle, kSystemPrompt, prompt.c_str());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

size_t GetMaxGeminiSystemPromptBytes() { return kMaxSystemPromptBytes; }

bool HasGeminiApiKey() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t size = 0;
    const esp_err_t result = nvs_get_str(handle, kApiKey, nullptr, &size);
    nvs_close(handle);
    return result == ESP_OK && size > 1 && size <= kMaxApiKeyLength + 1;
}

std::string GetGeminiApiKey() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return {};

    size_t size = 0;
    if (nvs_get_str(handle, kApiKey, nullptr, &size) != ESP_OK ||
        size <= 1 || size > kMaxApiKeyLength + 1) {
        nvs_close(handle);
        return {};
    }

    std::string value(size, '\0');
    const esp_err_t result = nvs_get_str(handle, kApiKey, value.data(), &size);
    nvs_close(handle);
    if (result != ESP_OK) return {};
    value.resize(size - 1);
    return value;
}

esp_err_t SaveGeminiApiKey(const std::string& api_key) {
    if (api_key.empty() || api_key.size() > kMaxApiKeyLength) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;

    result = nvs_set_str(handle, kApiKey, api_key.c_str());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

esp_err_t DeleteGeminiApiKey() {
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;

    result = nvs_erase_key(handle, kApiKey);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

}  // namespace chatai::settings
