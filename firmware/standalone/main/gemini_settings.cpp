#include "gemini_settings.h"

#include <nvs.h>
#include <algorithm>
#include <cstring>
#include <esp_log.h>

namespace {
constexpr char kNvsNamespace[] = "chatai_ai";
constexpr char kApiKey[] = "gemini_key";
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
