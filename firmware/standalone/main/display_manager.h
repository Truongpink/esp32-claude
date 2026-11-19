#pragma once

#include <cstdint>
#include <esp_err.h>

namespace chatai::display {
esp_err_t Initialize();
void SetAudioReady(bool ready);
void Notify(const char* title, const char* message, uint32_t duration_ms = 3000);
}  // namespace chatai::display
