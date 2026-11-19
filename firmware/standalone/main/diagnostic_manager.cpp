#include "diagnostic_manager.h"

#include <ctime>
#include <mutex>
#include <string>

#include <esp_log.h>

namespace {
constexpr char kTag[] = "ChatAI-Diag";
constexpr size_t kMaxLogBytes = 8192;
std::mutex g_mutex;
std::string g_log;
}

namespace chatai::diagnostics {

void Add(const char* stage, const char* result, const char* detail) {
    const std::time_t now = std::time(nullptr);
    std::tm time_info{};
    gmtime_r(&now, &time_info);
    char timestamp[24]{};
    if (now >= 1700000000) {
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &time_info);
    } else {
        std::snprintf(timestamp, sizeof(timestamp), "uptime-unknown");
    }

    std::string line = "[";
    line += timestamp;
    line += "][";
    line += stage != nullptr ? stage : "SYSTEM";
    line += "][";
    line += result != nullptr ? result : "INFO";
    line += "]";
    if (detail != nullptr && *detail != '\0') {
        line += " ";
        line += detail;
    }
    line += "\n";

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (line.size() >= kMaxLogBytes) line.resize(kMaxLogBytes - 1);
        if (g_log.size() + line.size() > kMaxLogBytes) {
            const size_t remove = g_log.size() + line.size() - kMaxLogBytes;
            const size_t next_line = g_log.find('\n', remove);
            g_log.erase(0, next_line == std::string::npos ? g_log.size() : next_line + 1);
        }
        g_log += line;
    }

    if (result != nullptr && (std::strcmp(result, "FAIL") == 0 ||
                              std::strcmp(result, "ERROR") == 0)) {
        ESP_LOGE(kTag, "%s", line.c_str());
    } else if (result != nullptr && std::strcmp(result, "WARN") == 0) {
        ESP_LOGW(kTag, "%s", line.c_str());
    } else {
        ESP_LOGI(kTag, "%s", line.c_str());
    }
}

std::string Get() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_log;
}

void Clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_log.clear();
}

}  // namespace chatai::diagnostics
