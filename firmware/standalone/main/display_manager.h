#pragma once

#include <esp_err.h>

namespace chatai::display {
esp_err_t Initialize();
void SetAudioReady(bool ready);
}  // namespace chatai::display
