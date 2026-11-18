#pragma once

#include <esp_err.h>
#include <string>

namespace chatai::settings {

esp_err_t MigrateLegacyGeminiApiKey();
bool HasGeminiApiKey();
std::string GetGeminiApiKey();
esp_err_t SaveGeminiApiKey(const std::string& api_key);
esp_err_t DeleteGeminiApiKey();

}  // namespace chatai::settings
