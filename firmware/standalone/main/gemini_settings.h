#pragma once

#include <esp_err.h>
#include <cstddef>
#include <string>

namespace chatai::settings {

esp_err_t MigrateLegacyGeminiApiKey();
bool HasGeminiApiKey();
std::string GetGeminiApiKey();
esp_err_t SaveGeminiApiKey(const std::string& api_key);
esp_err_t DeleteGeminiApiKey();

bool IsSupportedGeminiVoice(const std::string& voice);
std::string GetGeminiVoice();
esp_err_t SaveGeminiVoice(const std::string& voice);
std::string GetGeminiSystemPrompt();
esp_err_t SaveGeminiSystemPrompt(const std::string& prompt);
size_t GetMaxGeminiSystemPromptBytes();

}  // namespace chatai::settings
