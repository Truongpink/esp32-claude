#include "diagnostic_manager.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/time.h>

#include <esp_timer.h>
#include <esp_attr.h>
#include <esp_system.h>

#include <esp_log.h>

namespace {
constexpr char kTag[] = "ChatAI-Diag";
constexpr size_t kMaxLogBytes = 8192;
std::mutex g_mutex;
std::string g_log;
std::atomic<bool> g_time_synchronized{false};
std::atomic<const char*> g_time_source{"UNSYNCED"};

constexpr uint32_t kCrashContextMagic = 0x43484149;
struct RetainedCrashContext {
    uint32_t magic;
    uint32_t sequence;
    uint32_t checksum;
    char stage[24];
    char detail[80];
};
RTC_NOINIT_ATTR RetainedCrashContext g_retained_crash_context;
std::mutex g_crash_context_mutex;

uint32_t CrashContextChecksum(const RetainedCrashContext& context) {
    uint32_t hash = 2166136261u;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&context);
    for (size_t i = 0; i < sizeof(context); ++i) {
        if (i >= offsetof(RetainedCrashContext, checksum) &&
            i < offsetof(RetainedCrashContext, checksum) + sizeof(context.checksum)) continue;
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}

bool IsCrashContextValid(const RetainedCrashContext& context) {
    return context.magic == kCrashContextMagic &&
           context.checksum == CrashContextChecksum(context) &&
           context.stage[0] != '\0';
}

constexpr uint32_t kBootContextMagic = 0x424F4F54;
constexpr size_t kBootEventCount = 4;
struct BootEvent {
    uint32_t sequence;
    uint64_t uptime_ms;
    char stage[24];
    char detail[128];
};
struct RetainedBootContext {
    uint32_t magic;
    uint32_t checksum;
    uint8_t next_index;
    uint8_t count;
    BootEvent events[kBootEventCount];
};
RTC_NOINIT_ATTR RetainedBootContext g_retained_boot_context;
std::mutex g_boot_context_mutex;

uint32_t BootContextChecksum(const RetainedBootContext& context) {
    uint32_t hash = 2166136261u;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&context);
    for (size_t i = 0; i < sizeof(context); ++i) {
        if (i >= offsetof(RetainedBootContext, checksum) &&
            i < offsetof(RetainedBootContext, checksum) + sizeof(context.checksum)) continue;
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}

bool IsBootContextValid(const RetainedBootContext& context) {
    return context.magic == kBootContextMagic &&
           context.count > 0 && context.count <= kBootEventCount &&
           context.next_index < kBootEventCount &&
           context.checksum == BootContextChecksum(context);
}

void ClearBootContextLocked() {
    g_retained_boot_context.magic = 0;
    g_retained_boot_context.checksum = 0;
}

bool ShouldKeepDiagnostic(const char* stage, const char* result) {
    if (result != nullptr &&
        (std::strstr(result, "FAIL") != nullptr ||
         std::strstr(result, "ERROR") != nullptr ||
         std::strcmp(result, "WARN") == 0 ||
         std::strcmp(result, "RESET") == 0 ||
         std::strcmp(result, "DISCONNECTED") == 0)) {
        return true;
    }
    if (stage == nullptr) return false;
    if (std::strcmp(stage, "OLED") == 0) return true;
    if (std::strcmp(stage, "VOICE_LATENCY") == 0 ||
        std::strcmp(stage, "XIAOZHI_TTS") == 0 ||
        std::strcmp(stage, "XIAOZHI_LISTEN") == 0 ||
        std::strcmp(stage, "AFE_CAPTURE") == 0 ||
        std::strcmp(stage, "AFE") == 0 ||
        std::strcmp(stage, "MIC_IO") == 0 ||
        std::strcmp(stage, "MIC_LEVEL") == 0 ||
        std::strcmp(stage, "MIC_RAW") == 0 ||
        std::strcmp(stage, "MIC_VAD") == 0) {
        return true;
    }
    if (std::strncmp(stage, "BLE_", 4) == 0 ||
        std::strcmp(stage, "PREVIOUS_BOOT") == 0 ||
        std::strcmp(stage, "PREVIOUS_BOOT_EVENT") == 0) {
        return true;
    }
    if (std::strcmp(stage, "AMP_ENABLE") == 0 ||
        std::strcmp(stage, "SPEAKER_I2S") == 0 ||
        std::strcmp(stage, "SPEAKER_ENABLE") == 0 ||
        std::strcmp(stage, "OTA_CHECK") == 0 ||
        std::strcmp(stage, "ALERT_SOUND") == 0 ||
        std::strcmp(stage, "SOUND_PLAY") == 0) return true;
    if (std::strcmp(stage, "BOOT") == 0 && result != nullptr &&
        std::strcmp(result, "INFO") == 0) return true;
    if (std::strcmp(stage, "WIFI") == 0 && result != nullptr &&
        std::strcmp(result, "OK") == 0) return true;
    if (std::strcmp(stage, "HTTP_SERVER") == 0 && result != nullptr &&
        (std::strcmp(result, "STARTED") == 0 ||
         std::strcmp(result, "ROUTES_READY") == 0)) return true;
    if (std::strcmp(stage, "XIAOZHI") == 0 && result != nullptr &&
        (std::strcmp(result, "STARTING") == 0 ||
         std::strcmp(result, "READY") == 0)) return true;
    return false;
}
}

namespace chatai::diagnostics {

void SetTimeSyncStatus(bool synchronized, const char* source) {
    g_time_synchronized.store(synchronized);
    g_time_source.store(synchronized && source != nullptr ? source : "UNSYNCED");
}

void Add(const char* stage, const char* result, const char* detail) {
    if (!ShouldKeepDiagnostic(stage, result)) return;

    timeval wall_time{};
    const bool wall_time_valid =
        gettimeofday(&wall_time, nullptr) == 0 &&
        wall_time.tv_sec >= 1735689600 && wall_time.tv_sec < 4102444800;
    const bool time_synchronized = g_time_synchronized.load() && wall_time_valid;
    char real_time[96] = "UNSYNCED";
    if (time_synchronized) {
        const std::time_t ict_seconds = wall_time.tv_sec + 7 * 60 * 60;
        std::tm ict_time{};
        if (gmtime_r(&ict_seconds, &ict_time) != nullptr) {
            std::snprintf(real_time, sizeof(real_time),
                          "%04d-%02d-%02d %02d:%02d:%02d.%03ld ICT",
                          ict_time.tm_year + 1900, ict_time.tm_mon + 1,
                          ict_time.tm_mday, ict_time.tm_hour, ict_time.tm_min,
                          ict_time.tm_sec, static_cast<long>(wall_time.tv_usec / 1000));
        }
    }

    std::string line = "[TIME_SYNC=";
    line += time_synchronized ? "SYNCED" : "UNSYNCED";
    line += "][TIME_REAL=";
    line += real_time;
    line += "][UPTIME_MS=";
    line += std::to_string(static_cast<long long>(esp_timer_get_time() / 1000));
    line += "]";
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
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_log.clear();
    }
    ClearCrashContext();
    std::lock_guard<std::mutex> boot_lock(g_boot_context_mutex);
    ClearBootContextLocked();
}

void SetCrashContext(const char* stage, const char* detail) {
    if (stage == nullptr || *stage == '\0') return;
    std::lock_guard<std::mutex> lock(g_crash_context_mutex);
    RetainedCrashContext next{};
    next.magic = kCrashContextMagic;
    next.sequence = IsCrashContextValid(g_retained_crash_context)
        ? g_retained_crash_context.sequence + 1 : 1;
    std::snprintf(next.stage, sizeof(next.stage), "%s", stage);
    if (detail != nullptr) {
        std::snprintf(next.detail, sizeof(next.detail), "%s", detail);
    }
    next.checksum = CrashContextChecksum(next);
    g_retained_crash_context = next;
}

void ClearCrashContext() {
    std::lock_guard<std::mutex> lock(g_crash_context_mutex);
    g_retained_crash_context.magic = 0;
    g_retained_crash_context.checksum = 0;
}

void SetBootContext(const char* stage, const char* detail) {
    if (stage == nullptr || *stage == '\0') return;
    std::lock_guard<std::mutex> lock(g_boot_context_mutex);
    RetainedBootContext next{};
    if (IsBootContextValid(g_retained_boot_context)) {
        next = g_retained_boot_context;
    } else {
        next.magic = kBootContextMagic;
    }
    BootEvent& event = next.events[next.next_index];
    event.sequence = next.count == 0 ? 1 :
        (next.next_index == 0 ? next.events[kBootEventCount - 1].sequence + 1 :
                               next.events[next.next_index - 1].sequence + 1);
    event.uptime_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    std::snprintf(event.stage, sizeof(event.stage), "%s", stage);
    if (detail != nullptr) {
        std::snprintf(event.detail, sizeof(event.detail), "%s", detail);
    }
    next.next_index = static_cast<uint8_t>((next.next_index + 1) % kBootEventCount);
    if (next.count < kBootEventCount) ++next.count;
    next.magic = kBootContextMagic;
    next.checksum = BootContextChecksum(next);
    g_retained_boot_context = next;
}

void RestorePreviousCrashContext() {
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool retained_reset = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
        reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
        reason == ESP_RST_SW || reason == ESP_RST_EXT ||
        reason == ESP_RST_BROWNOUT;
    if (retained_reset) {
        RetainedCrashContext previous{};
        bool valid = false;
        {
            std::lock_guard<std::mutex> lock(g_crash_context_mutex);
            if (IsCrashContextValid(g_retained_crash_context)) {
                previous = g_retained_crash_context;
                valid = true;
            }
            g_retained_crash_context.magic = 0;
            g_retained_crash_context.checksum = 0;
        }
        if (valid) {
            char detail[144]{};
            std::snprintf(detail, sizeof(detail),
                          "previous_reset_reason=%d last_stage=%.23s sequence=%lu %.60s",
                          static_cast<int>(reason), previous.stage,
                          static_cast<unsigned long>(previous.sequence), previous.detail);
            Add("PREVIOUS_BOOT", "WARN", detail);
        }
    } else {
        ClearCrashContext();
    }

    RetainedBootContext previous_boot{};
    bool boot_valid = false;
    {
        std::lock_guard<std::mutex> lock(g_boot_context_mutex);
        if (retained_reset && IsBootContextValid(g_retained_boot_context)) {
            previous_boot = g_retained_boot_context;
            boot_valid = true;
        }
        ClearBootContextLocked();
    }
    if (boot_valid) {
        const size_t first = (previous_boot.next_index + kBootEventCount -
                              previous_boot.count) % kBootEventCount;
        for (size_t i = 0; i < previous_boot.count; ++i) {
            const BootEvent& event =
                previous_boot.events[(first + i) % kBootEventCount];
            char detail[192]{};
            std::snprintf(detail, sizeof(detail),
                          "event_seq=%lu uptime_ms=%llu stage=%.23s %.120s",
                          static_cast<unsigned long>(event.sequence),
                          static_cast<unsigned long long>(event.uptime_ms),
                          event.stage, event.detail);
            Add("PREVIOUS_BOOT_EVENT", "WARN", detail);
        }
    }
}

}  // namespace chatai::diagnostics
