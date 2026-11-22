#pragma once

#include <esp_err.h>
#include <cstdint>
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
esp_err_t SavePendingFirmwareName(const std::string& filename, uint32_t target_address);
esp_err_t ConfirmPendingFirmwareName(uint32_t running_address);
bool ConsumePendingFirmwareRollback(uint32_t running_address);
std::string GetInstalledFirmwareName();

}  // namespace chatai::settings
