#include "diagnostic_manager.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include <esp_timer.h>

#include <esp_log.h>

namespace {
constexpr char kTag[] = "ChatAI-Diag";
constexpr size_t kMaxLogBytes = 8192;
std::mutex g_mutex;
std::string g_log;
std::atomic<bool> g_time_synchronized{false};
std::atomic<const char*> g_time_source{"UNSYNCED"};
}

namespace chatai::diagnostics {

void SetTimeSyncStatus(bool synchronized, const char* source) {
    g_time_synchronized.store(synchronized);
    g_time_source.store(synchronized && source != nullptr ? source : "UNSYNCED");
}

void Add(const char* stage, const char* result, const char* detail) {
    constexpr std::time_t kMinimumValidTime = 1700000000;
    constexpr std::time_t kVietnamOffsetSeconds = 7 * 60 * 60;
    const std::time_t now = std::time(nullptr);
    const bool epoch_valid = now >= kMinimumValidTime;
    const bool synchronized = epoch_valid && g_time_synchronized.load();
    char utc_timestamp[24]{};
    char ict_timestamp[24]{};
    if (epoch_valid) {
        std::tm utc_info{};
        std::tm ict_info{};
        gmtime_r(&now, &utc_info);
        const std::time_t ict_epoch = now + kVietnamOffsetSeconds;
        gmtime_r(&ict_epoch, &ict_info);
        std::strftime(utc_timestamp, sizeof(utc_timestamp), "%Y-%m-%d %H:%M:%S", &utc_info);
        std::strftime(ict_timestamp, sizeof(ict_timestamp), "%Y-%m-%d %H:%M:%S", &ict_info);
    }

    std::string line;
    if (epoch_valid) {
        line = "[";
        line += ict_timestamp;
        line += " ICT][";
        line += utc_timestamp;
        line += " UTC][UNIX=";
        line += std::to_string(static_cast<long long>(now));
        line += "][TIME_SYNC=";
        line += synchronized ? g_time_source.load() : "UNVERIFIED";
        line += "]";
    } else {
        line = "[TIME_UNSYNCED][UPTIME_MS=";
        line += std::to_string(static_cast<long long>(esp_timer_get_time() / 1000));
        line += "][TIME_SYNC=NO]";
    }
    line += "[";
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
