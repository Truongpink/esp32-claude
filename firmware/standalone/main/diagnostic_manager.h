#pragma once

#include <string>

namespace chatai::diagnostics {
void SetTimeSyncStatus(bool synchronized, const char* source = nullptr);
void Add(const char* stage, const char* result, const char* detail = nullptr);
std::string Get();
void Clear();
}
