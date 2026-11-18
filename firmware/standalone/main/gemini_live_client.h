#pragma once

#include <esp_err.h>

namespace chatai::gemini {

esp_err_t Initialize();
bool IsReady();

}  // namespace chatai::gemini
