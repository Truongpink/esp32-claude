#include "display_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iterator>
#include <mutex>
#include <string_view>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "chatai_xiaozhi_client.h"
#include "network_manager.h"
#include "diagnostic_manager.h"
#include "vietnamese_font.h"

namespace {
constexpr char kTag[] = "ChatAI-OLED";
constexpr gpio_num_t kSdaPin = GPIO_NUM_41;
constexpr gpio_num_t kSclPin = GPIO_NUM_42;
constexpr uint16_t kAddress = 0x3C;
constexpr int kWidth = 128;
constexpr size_t kPages = 8;
constexpr int kGlyphWidth = 8;
constexpr int kGlyphHeight = 12;
constexpr int kFooterY = 52;
constexpr TickType_t kScrollStepTicks = pdMS_TO_TICKS(120);

i2c_master_bus_handle_t g_bus = nullptr;
i2c_master_dev_handle_t g_display = nullptr;
std::atomic<bool> g_initialized{false};
bool g_display_task_started = false;
bool g_first_full_refresh_logged = false;
bool g_bus_reset_required = false;
int64_t g_last_init_error_log_us = 0;
std::atomic<bool> g_sntp_started{false};
std::atomic<bool> g_time_synced{false};
std::atomic<bool> g_audio_ready{false};
std::mutex g_content_mutex;
char g_chat_message[96]{};
std::array<chatai::display::ChatTranscriptEntry, chatai::display::kChatTranscriptCapacity> g_chat_history{};
size_t g_chat_history_count = 0;
size_t g_chat_history_next = 0;
std::atomic<uint8_t> g_bluetooth_status{
    static_cast<uint8_t>(chatai::display::BluetoothStatus::kIdle)};
std::atomic<size_t> g_bluetooth_device_count{0};
std::atomic<int> g_bluetooth_error{0};
std::atomic<int64_t> g_bluetooth_status_until_us{0};
std::array<uint8_t, kWidth * kPages> g_frame{};
std::array<uint8_t, kWidth * kPages> g_last_flushed_frame{};
bool g_frame_flushed = false;
int64_t g_last_full_refresh_us = 0;
int64_t g_next_i2c_recovery_us = 0;
int64_t g_last_i2c_error_log_us = 0;
char g_footer[96] = "CHATAI TPIOS - WIFI WAIT - SERVER WAIT";
int g_footer_x = -static_cast<int>(sizeof("CHATAI TPIOS - WIFI WAIT - SERVER WAIT") - 1) * kGlyphWidth;
TickType_t g_last_scroll_tick = 0;

void LogOled(const char* result, const char* event, const char* detail = nullptr) {
    char message[192]{};
    std::snprintf(message, sizeof(message), "%s%s%s",
                  event != nullptr ? event : "OLED",
                  detail != nullptr && *detail != '\0' ? " " : "",
                  detail != nullptr ? detail : "");
    chatai::diagnostics::Add("OLED", result, message);
}

void LogOledInitFailure(const char* event, esp_err_t error) {
    const int64_t now_us = esp_timer_get_time();
    if (g_last_init_error_log_us != 0 && now_us - g_last_init_error_log_us < 5000000) return;
    g_last_init_error_log_us = now_us;
    char detail[128]{};
    std::snprintf(detail, sizeof(detail), "%s error=%s(%d)",
                  event, esp_err_to_name(error), static_cast<int>(error));
    LogOled("FAIL", event, detail);
}

void LogI2cLineLevels(const char* stage) {
    const int sda_level = gpio_get_level(kSdaPin);
    const int scl_level = gpio_get_level(kSclPin);
    char detail[112]{};
    std::snprintf(detail, sizeof(detail),
                  "stage=%s SDA_GPIO41=%d SCL_GPIO42=%d",
                  stage != nullptr ? stage : "unknown", sda_level, scl_level);
    LogOled("INFO", "I2C_LINE_LEVELS", detail);
}

uint32_t DecodeUtf8(std::string_view text, size_t& offset) {
    const auto first = static_cast<uint8_t>(text[offset++]);
    if (first < 0x80) return first;
    if ((first & 0xE0) == 0xC0 && offset < text.size()) {
        const auto second = static_cast<uint8_t>(text[offset++]);
        return ((first & 0x1F) << 6) | (second & 0x3F);
    }
    if ((first & 0xF0) == 0xE0 && offset + 1 < text.size()) {
        const auto second = static_cast<uint8_t>(text[offset++]);
        const auto third = static_cast<uint8_t>(text[offset++]);
        return ((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);
    }
    if ((first & 0xF8) == 0xF0 && offset + 2 < text.size()) {
        const auto second = static_cast<uint8_t>(text[offset++]);
        const auto third = static_cast<uint8_t>(text[offset++]);
        const auto fourth = static_cast<uint8_t>(text[offset++]);
        return ((first & 0x07) << 18) | ((second & 0x3F) << 12) |
               ((third & 0x3F) << 6) | (fourth & 0x3F);
    }
    return '?';
}

const chatai::display::Glyph8x12* FindGlyph(uint32_t codepoint) {
    const auto begin = std::begin(chatai::display::kVietnameseFont);
    const auto end = std::end(chatai::display::kVietnameseFont);
    const auto found = std::lower_bound(
        begin, end, codepoint,
        [](const chatai::display::Glyph8x12& glyph, uint32_t value) {
            return glyph.codepoint < value;
        });
    return found != end && found->codepoint == codepoint ? &*found : nullptr;
}

bool IsVietnameseCombiningMark(uint32_t codepoint) {
    return codepoint == 0x0300 || codepoint == 0x0301 ||
           codepoint == 0x0303 || codepoint == 0x0306 ||
           codepoint == 0x0309 || codepoint == 0x031B ||
           codepoint == 0x0323;
}

uint32_t ComposeVietnamese(uint32_t base, uint32_t mark) {
    const auto begin = std::begin(chatai::display::kVietnameseCompositions);
    const auto end = std::end(chatai::display::kVietnameseCompositions);
    const uint64_t key = (static_cast<uint64_t>(base) << 32) | mark;
    const auto found = std::lower_bound(
        begin, end, key,
        [](const chatai::display::UnicodeComposition& item, uint64_t value) {
            return ((static_cast<uint64_t>(item.base) << 32) | item.mark) < value;
        });
    return found != end && found->base == base && found->mark == mark
        ? found->composed : base;
}

// Proportional advances make the compact OLED typeface feel closer to SF Pro.
// The bitmap remains a fixed 8x12 raster, so rendering needs no allocation.
int GlyphBounds(const chatai::display::Glyph8x12& glyph, int& min_column, int& max_column) {
    min_column = kGlyphWidth;
    max_column = -1;
    for (int row = 0; row < kGlyphHeight; ++row) {
        for (int column = 0; column < kGlyphWidth; ++column) {
            if ((glyph.rows[row] & (1U << (7 - column))) != 0) {
                min_column = std::min(min_column, column);
                max_column = std::max(max_column, column);
            }
        }
    }
    return max_column >= min_column ? max_column + 1 : 4;
}

// Decomposed Vietnamese tone marks are composed before lookup so NFC and NFD
// input render identically.
void DrawText(int x, int y, std::string_view text) {
    size_t offset = 0;
    while (offset < text.size()) {
        uint32_t codepoint = DecodeUtf8(text, offset);
        if (IsVietnameseCombiningMark(codepoint)) continue;

        while (offset < text.size()) {
            const size_t next_offset = offset;
            const uint32_t mark = DecodeUtf8(text, offset);
            if (!IsVietnameseCombiningMark(mark)) {
                offset = next_offset;
                break;
            }
            codepoint = ComposeVietnamese(codepoint, mark);
        }

        const auto* glyph = FindGlyph(codepoint);
        if (glyph != nullptr) {
            int min_column = 0;
            int max_column = -1;
            const int glyph_width = GlyphBounds(*glyph, min_column, max_column);
            if (max_column >= min_column) {
                for (int row = 0; row < kGlyphHeight; ++row) {
                    for (int column = min_column; column <= max_column; ++column) {
                        if ((glyph->rows[row] & (1U << (7 - column))) == 0) continue;
                        const int pixel_x = x + column;
                        const int pixel_y = y + row;
                        if (pixel_x >= 0 && pixel_x < kWidth &&
                            pixel_y >= 0 && pixel_y < 64) {
                            g_frame[(pixel_y / 8) * kWidth + pixel_x] |=
                                static_cast<uint8_t>(1U << (pixel_y % 8));
                        }
                    }
                }
            }
            x += glyph_width + 1;
        } else {
            x += 6;
        }
    }
}

int TextWidth(std::string_view text) {
    size_t offset = 0;
    int width = 0;
    while (offset < text.size()) {
        const uint32_t codepoint = DecodeUtf8(text, offset);
        if (IsVietnameseCombiningMark(codepoint)) continue;
        const auto* glyph = FindGlyph(codepoint);
        if (glyph == nullptr) {
            width += 6;
            continue;
        }
        int min_column = 0;
        int max_column = -1;
        width += GlyphBounds(*glyph, min_column, max_column) + 1;
    }
    return width > 0 ? width - 1 : 0;
}

void SetPixel(int x, int y) {
    if (x < 0 || x >= kWidth || y < 0 || y >= 64) return;
    g_frame[(y / 8) * kWidth + x] |= static_cast<uint8_t>(1U << (y % 8));
}

void DrawWifiIcon() {
    constexpr std::array<uint16_t, 10> rows{
        0b000000000000,
        0b000111111000,
        0b011000000110,
        0b100000000001,
        0b000011110000,
        0b000100001000,
        0b001000000100,
        0b000001100000,
        0b000001100000,
        0b000000000000
    };
    constexpr int x = 2;
    constexpr int y = 1;
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        for (int column = 0; column < 12; ++column) {
            if ((rows[row] & (1U << (11 - column))) != 0) {
                SetPixel(x + column, y + row);
            }
        }
    }
}

void DrawBatteryIcon() {
    constexpr int x = 108;
    constexpr int y = 2;
    constexpr int body_width = 16;
    constexpr int body_height = 8;
    for (int column = 0; column < body_width; ++column) {
        SetPixel(x + column, y);
        SetPixel(x + column, y + body_height - 1);
    }
    for (int row = 1; row < body_height - 1; ++row) {
        SetPixel(x, y + row);
        SetPixel(x + body_width - 1, y + row);
    }
    for (int row = 2; row < 6; ++row) {
        SetPixel(x + body_width, y + row);
    }
}

void UpdateClockSync() {
    if (!chatai::network::NetworkManager::IsConnected()) return;

    if (!g_sntp_started.exchange(true)) {
        // POSIX TZ offsets use the opposite sign: UTC-7 means Vietnam (UTC+7).
        setenv("TZ", "UTC-7", 1);
        tzset();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        ESP_LOGI(kTag, "SNTP started after Wi-Fi connection; timezone=UTC+7");
    }

    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        g_time_synced = true;
    }
}

void LogClockSnapshot(const char* stage, std::time_t now) {
    char utc_text[24] = "INVALID";
    char local_text[24] = "INVALID";
    if (now >= 1700000000) {
        std::tm utc_time{};
        std::tm local_time{};
        if (gmtime_r(&now, &utc_time) != nullptr) {
            std::strftime(utc_text, sizeof(utc_text), "%Y-%m-%d %H:%M:%S", &utc_time);
        }
        if (localtime_r(&now, &local_time) != nullptr) {
            std::strftime(local_text, sizeof(local_text), "%Y-%m-%d %H:%M:%S", &local_time);
        }
    }
    const char* timezone = std::getenv("TZ");
    ESP_LOGI(kTag,
             "CLOCK_CHECK stage=%s epoch=%lld utc=%s local=%s tz=%s sntp_status=%d synced=%d",
             stage,
             static_cast<long long>(now),
             utc_text,
             local_text,
             timezone != nullptr ? timezone : "(unset)",
             static_cast<int>(esp_sntp_get_sync_status()),
             g_time_synced ? 1 : 0);
}

bool g_clock_first_snapshot_logged = false;
bool g_clock_sync_snapshot_logged = false;

void DrawClock() {
    if (!g_clock_first_snapshot_logged) {
        LogClockSnapshot("first_display", std::time(nullptr));
        g_clock_first_snapshot_logged = true;
    }

    UpdateClockSync();

    const std::time_t now = std::time(nullptr);
    if (g_time_synced && !g_clock_sync_snapshot_logged) {
        LogClockSnapshot("ntp_completed", now);
        g_clock_sync_snapshot_logged = true;
    }

    char clock_text[9] = "--:--:--";
    if (g_time_synced && now >= 1700000000) {
        std::tm local_time{};
        if (localtime_r(&now, &local_time) != nullptr) {
            std::strftime(clock_text, sizeof(clock_text), "%H:%M:%S", &local_time);
        }
    }
    DrawText((kWidth - TextWidth(clock_text)) / 2, 1, clock_text);
}

void UpdateFooter() {
    const bool wifi_ready = chatai::network::NetworkManager::IsConnected();
    const std::string ip_address = wifi_ready
        ? chatai::network::NetworkManager::GetIpAddress()
        : std::string{};
    const bool has_ip = !ip_address.empty() && ip_address != "0.0.0.0";
    const bool server_ready = chatai::xiaozhi::IsReady();
    char next[sizeof(g_footer)]{};
    std::snprintf(next, sizeof(next), "CHATAI TPIOS - IP: %s - SERVER %s",
                  has_ip ? ip_address.c_str() : "WAIT",
                  server_ready ? "READY" : "WAIT");
    std::lock_guard<std::mutex> lock(g_content_mutex);
    if (std::strcmp(next, g_footer) == 0) return;
    std::snprintf(g_footer, sizeof(g_footer), "%s", next);
    g_footer_x = -TextWidth(g_footer);
}

const char* StatusText(chatai::xiaozhi::RuntimeState state) {
    using chatai::xiaozhi::RuntimeState;
    switch (state) {
        case RuntimeState::kConnecting: return "ĐANG KẾT NỐI";
        case RuntimeState::kWaiting: return "ĐANG CHỜ";
        case RuntimeState::kListening: return "ĐANG NGHE";
        case RuntimeState::kProcessing: return "ĐANG XỬ LÝ";
        case RuntimeState::kSpeaking: return "ĐANG NÓI";
        case RuntimeState::kError: return "LỖI MICROPHONE";
    }
    return "ĐANG KẾT NỐI";
}

esp_err_t InitializeController() {
    const uint8_t initialization[] = {
        0x00, 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00,
        0x40, 0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8, 0xDA,
        0x12, 0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA6, 0xAF
    };
    return i2c_master_transmit(g_display, initialization, sizeof(initialization), 250);
}

esp_err_t RefreshController() {
    // Reapply panel power, addressing, scan and display-on settings without blanking first.
    const uint8_t refresh[] = {
        0x00, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA6, 0xAF
    };
    return i2c_master_transmit(g_display, refresh, sizeof(refresh), 250);
}

esp_err_t FlushFrame() {
    constexpr int64_t kFullRefreshIntervalUs = 5000000;
    const int64_t now_us = esp_timer_get_time();
    const bool force_full_refresh = !g_frame_flushed ||
        now_us - g_last_full_refresh_us >= kFullRefreshIntervalUs;

    if (force_full_refresh) {
        // Restore SSD1306 configuration in case its controller reset silently.
        const esp_err_t refresh_result = RefreshController();
        if (refresh_result != ESP_OK) return refresh_result;
        if (!g_first_full_refresh_logged) {
            LogOled("OK", "OLED_FULL_REFRESH", "controller state reapplied");
            g_first_full_refresh_logged = true;
        }
    }

    for (uint8_t page = 0; page < kPages; ++page) {
        const size_t offset = page * kWidth;
        const uint8_t* current = g_frame.data() + offset;
        if (!force_full_refresh && g_frame_flushed &&
            std::memcmp(current, g_last_flushed_frame.data() + offset, kWidth) == 0) {
            continue;
        }

        const uint8_t position[] = {
            0x00, static_cast<uint8_t>(0xB0 | page), 0x00, 0x10
        };
        esp_err_t result = i2c_master_transmit(g_display, position, sizeof(position), 100);
        if (result != ESP_OK) return result;

        uint8_t packet[kWidth + 1]{};
        packet[0] = 0x40;
        std::memcpy(packet + 1, current, kWidth);
        result = i2c_master_transmit(g_display, packet, sizeof(packet), 250);
        if (result != ESP_OK) return result;
        std::memcpy(g_last_flushed_frame.data() + offset, current, kWidth);
    }
    g_frame_flushed = true;
    if (force_full_refresh) g_last_full_refresh_us = now_us;
    return ESP_OK;
}

void RenderStatus() {
    g_frame.fill(0);
    UpdateFooter();

    if (chatai::network::NetworkManager::IsConnected()) {
        DrawWifiIcon();
    }
    DrawClock();
    DrawBatteryIcon();

    const auto bluetooth_status = static_cast<chatai::display::BluetoothStatus>(
        g_bluetooth_status.load());
    const int64_t bluetooth_until = g_bluetooth_status_until_us.load();
    const bool show_bluetooth =
        bluetooth_status == chatai::display::BluetoothStatus::kInitializing ||
        bluetooth_status == chatai::display::BluetoothStatus::kScanning ||
        (bluetooth_until > 0 && esp_timer_get_time() < bluetooth_until);

    if (show_bluetooth) {
        const char* state_text = "BLE READY";
        switch (bluetooth_status) {
            case chatai::display::BluetoothStatus::kInitializing:
                state_text = "BLE INIT";
                break;
            case chatai::display::BluetoothStatus::kReady:
                state_text = "BLE READY";
                break;
            case chatai::display::BluetoothStatus::kScanning:
                state_text = "BLE SCANNING";
                break;
            case chatai::display::BluetoothStatus::kComplete:
                state_text = "SCAN COMPLETE";
                break;
            case chatai::display::BluetoothStatus::kError:
                state_text = "BLE ERROR";
                break;
            case chatai::display::BluetoothStatus::kIdle:
                break;
        }
        DrawText((kWidth - TextWidth("BLUETOOTH")) / 2, 17, "BLUETOOTH");
        DrawText((kWidth - TextWidth(state_text)) / 2, 29, state_text);
        char device_text[24]{};
        std::snprintf(device_text, sizeof(device_text), "DEVICES: %u",
                      static_cast<unsigned>(g_bluetooth_device_count.load()));
        DrawText((kWidth - TextWidth(device_text)) / 2, 41, device_text);
        if (bluetooth_status == chatai::display::BluetoothStatus::kError) {
            char error_text[20]{};
            std::snprintf(error_text, sizeof(error_text), "CODE: %d",
                          g_bluetooth_error.load());
            DrawText((kWidth - TextWidth(error_text)) / 2, 41, error_text);
        }
    } else {
        char chat_message[sizeof(g_chat_message)]{};
        {
            std::lock_guard<std::mutex> lock(g_content_mutex);
            std::snprintf(chat_message, sizeof(chat_message), "%s", g_chat_message);
        }
        // Read the live XiaoZhi state every frame; cached SetStatus text can go stale.
        const char* status = !g_audio_ready.load()
            ? "LỖI ÂM THANH"
            : StatusText(chatai::xiaozhi::GetDisplayRuntimeState());
        DrawText((kWidth - TextWidth(status)) / 2, 20, status);
        if (chat_message[0] != '\0') {
            DrawText((kWidth - TextWidth(chat_message)) / 2, 34, chat_message);
        }

        for (int x = 0; x < kWidth; x += 2) {
            const int y = 46;
            g_frame[(y / 8) * kWidth + x] |= static_cast<uint8_t>(1U << (y % 8));
        }
    }

    const TickType_t now = xTaskGetTickCount();
    if (static_cast<TickType_t>(now - g_last_scroll_tick) >= kScrollStepTicks) {
        ++g_footer_x;
        g_last_scroll_tick = now;
        if (g_footer_x > kWidth) g_footer_x = -TextWidth(g_footer);
    }
    DrawText(g_footer_x, kFooterY, g_footer);

    static bool first_frame_logged = false;
    const esp_err_t result = FlushFrame();
    if (result != ESP_OK) {
        g_frame_flushed = false;
        const int64_t now_us = esp_timer_get_time();
        if (g_last_i2c_error_log_us == 0 ||
            now_us - g_last_i2c_error_log_us >= 5000000) {
            ESP_LOGE(kTag, "OLED framebuffer transfer failed: %s",
                     esp_err_to_name(result));
            char detail[96]{};
            std::snprintf(detail, sizeof(detail), "framebuffer transfer error=%s(%d)",
                          esp_err_to_name(result), static_cast<int>(result));
            LogOled("FAIL", "OLED_FRAME_TRANSFER", detail);
            g_last_i2c_error_log_us = now_us;
        }

        // Recover only the OLED's I2C bus; never restart the ESP32 or audio subsystem.
        if (now_us >= g_next_i2c_recovery_us) {
            g_next_i2c_recovery_us = now_us + 5000000;
            LogOled("WARN", "OLED_RECOVERY_START", "resetting I2C bus and reinitializing SSD1306");
            const esp_err_t reset_result = i2c_master_bus_reset(g_bus);
            if (reset_result == ESP_OK) {
                const esp_err_t init_result = InitializeController();
                if (init_result == ESP_OK) {
                    g_last_full_refresh_us = 0;
                    g_frame_flushed = false;
                    LogOled("OK", "OLED_RECOVERY_RESULT", "I2C bus reset and controller initialized");
                    ESP_LOGW(kTag, "OLED I2C bus and SSD1306 controller recovered");
                } else {
                    g_initialized = false;
                    LogOledInitFailure("OLED_RECOVERY_CONTROLLER_FAILED", init_result);
                    ESP_LOGE(kTag, "OLED controller reinitialization failed: %s",
                             esp_err_to_name(init_result));
                }
            } else {
                g_initialized = false;
                g_bus_reset_required = true;
                LogOledInitFailure("OLED_RECOVERY_BUS_FAILED", reset_result);
                ESP_LOGE(kTag, "OLED I2C bus recovery failed: %s",
                         esp_err_to_name(reset_result));
            }
        }
    } else if (!first_frame_logged) {
        LogOled("OK", "OLED_FIRST_FRAME", "128x64 framebuffer transmitted");
        ESP_LOGI(kTag, "OLED first framebuffer transfer completed");
        first_frame_logged = true;
    }
}

bool InitializeHardware() {
    if (g_bus == nullptr) {
        i2c_master_bus_config_t bus_config{};
        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = kSdaPin;
        bus_config.scl_io_num = kSclPin;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.flags.enable_internal_pullup = true;

        const esp_err_t bus_result = i2c_new_master_bus(&bus_config, &g_bus);
        if (bus_result != ESP_OK) {
            LogOledInitFailure("I2C_BUS_INIT_FAILED", bus_result);
            g_bus = nullptr;
            return false;
        }
        LogOled("OK", "I2C_BUS_READY", "port=I2C_NUM_0 SDA=41 SCL=42");
    }

    if (g_display == nullptr) {
        // If a previous recovery failed, restore the bus before attempting another probe.
        if (g_bus_reset_required) {
            const esp_err_t reset_result = i2c_master_bus_reset(g_bus);
            char reset_detail[96]{};
            std::snprintf(reset_detail, sizeof(reset_detail),
                          "stage=pre_probe result=%s(%d)",
                          esp_err_to_name(reset_result), static_cast<int>(reset_result));
            LogOled(reset_result == ESP_OK ? "OK" : "FAIL",
                    "I2C_BUS_RESET", reset_detail);
            if (reset_result != ESP_OK) {
                LogI2cLineLevels("pre_probe_reset_failed");
                LogOledInitFailure("OLED_RETRY_BUS_RESET_FAILED", reset_result);
                return false;
            }
            g_bus_reset_required = false;
        }

        LogI2cLineLevels("before_probe");
        esp_err_t probe_result = i2c_master_probe(g_bus, kAddress, 100);
        if (probe_result == ESP_ERR_TIMEOUT) {
            LogI2cLineLevels("probe_timeout");
            // The IDF master driver exposes bus reset to recover a busy/stuck bus.
            const esp_err_t reset_result = i2c_master_bus_reset(g_bus);
            char reset_detail[96]{};
            std::snprintf(reset_detail, sizeof(reset_detail),
                          "stage=probe_timeout result=%s(%d)",
                          esp_err_to_name(reset_result), static_cast<int>(reset_result));
            LogOled(reset_result == ESP_OK ? "WARN" : "FAIL",
                    "I2C_BUS_RESET", reset_detail);
            LogI2cLineLevels("after_probe_reset");
            if (reset_result == ESP_OK) {
                probe_result = i2c_master_probe(g_bus, kAddress, 100);
                LogI2cLineLevels(probe_result == ESP_OK
                    ? "probe_retry_ok" : "probe_retry_failed");
            } else {
                g_bus_reset_required = true;
            }
        }
        if (probe_result != ESP_OK) {
            LogOledInitFailure("OLED_ADDRESS_CHECK_FAILED", probe_result);
            return false;
        }
        LogOled("OK", "OLED_ADDRESS_CHECK", "SSD1306 ACK at 0x3C");

        i2c_device_config_t device_config{};
        device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device_config.device_address = kAddress;
        device_config.scl_speed_hz = 400000;
        const esp_err_t add_result =
            i2c_master_bus_add_device(g_bus, &device_config, &g_display);
        if (add_result != ESP_OK) {
            LogOledInitFailure("OLED_DEVICE_REGISTER_FAILED", add_result);
            g_display = nullptr;
            return false;
        }
    }

    const esp_err_t controller_result = InitializeController();
    if (controller_result != ESP_OK) {
        LogOledInitFailure("OLED_CONTROLLER_INIT_FAILED", controller_result);
        return false;
    }

    g_initialized = true;
    g_frame_flushed = false;
    g_last_full_refresh_us = 0;
    g_last_scroll_tick = xTaskGetTickCount();
    LogOled("OK", "OLED_CONTROLLER_INIT", "SSD1306 initialization sequence accepted");
    LogOled("OK", "OLED_DISPLAY_ON", "SSD1306 command 0xAF transmitted");
    return true;
}

void DisplayTask(void*) {
    while (true) {
        if (!g_initialized) {
            if (!InitializeHardware()) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }
        RenderStatus();
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}
}  // namespace

namespace chatai::display {

esp_err_t Initialize() {
    if (g_display_task_started) return ESP_OK;

    LogOled("INFO", "OLED_INIT_START",
            "SSD1306 128x64 address=0x3C SDA=41 SCL=42");
    if (xTaskCreate(DisplayTask, "chatai_oled", 4096, nullptr, 3, nullptr) != pdPASS) {
        LogOled("FAIL", "OLED_DISPLAY_TASK_FAILED", "could not create OLED supervisor task");
        ESP_LOGE(kTag, "OLED display task creation failed");
        return ESP_ERR_NO_MEM;
    }

    g_display_task_started = true;
    ESP_LOGI(kTag, "OLED supervisor task started; initialization will retry until successful");
    return ESP_OK;
}

void SetAudioReady(bool ready) {
    g_audio_ready.store(ready);
}

void SetChatMessage(const char* role, const char* message) {
    if (role == nullptr || message == nullptr ||
        (std::strcmp(role, "user") != 0 && std::strcmp(role, "assistant") != 0 &&
         std::strcmp(role, "system") != 0)) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_content_mutex);
    std::snprintf(g_chat_message, sizeof(g_chat_message), "%s", message);
    if ((std::strcmp(role, "user") == 0 || std::strcmp(role, "assistant") == 0) &&
        message[0] != '\0') {
        auto& entry = g_chat_history[g_chat_history_next];
        entry = {};
        entry.uptime_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
        std::snprintf(entry.role, sizeof(entry.role), "%s", role);
        std::snprintf(entry.message, sizeof(entry.message), "%s", message);
        g_chat_history_next = (g_chat_history_next + 1) % chatai::display::kChatTranscriptCapacity;
        if (g_chat_history_count < chatai::display::kChatTranscriptCapacity) ++g_chat_history_count;
    }
}

void ClearChatMessages() {
    std::lock_guard<std::mutex> lock(g_content_mutex);
    g_chat_message[0] = '\0';
}

void GetWebSnapshot(WebSnapshot& snapshot) {
    snapshot = WebSnapshot{};
    snapshot.oled_ready = g_initialized.load();
    // Keep Web Admin's clock/footer current without mutating the OLED scroll state from HTTP.
    UpdateClockSync();
    const std::time_t now = std::time(nullptr);
    if (g_time_synced.load() && now >= 1700000000) {
        std::tm local_time{};
        if (localtime_r(&now, &local_time) != nullptr) {
            std::strftime(snapshot.clock, sizeof(snapshot.clock), "%H:%M:%S", &local_time);
        }
    }
    const bool wifi_ready = chatai::network::NetworkManager::IsConnected();
    const std::string ip_address = wifi_ready
        ? chatai::network::NetworkManager::GetIpAddress()
        : std::string{};
    const bool has_ip = !ip_address.empty() && ip_address != "0.0.0.0";
    std::snprintf(snapshot.footer, sizeof(snapshot.footer),
                  "CHATAI TPIOS - IP: %s - SERVER %s",
                  has_ip ? ip_address.c_str() : "WAIT",
                  chatai::xiaozhi::IsReady() ? "READY" : "WAIT");
    std::lock_guard<std::mutex> lock(g_content_mutex);
    std::snprintf(snapshot.chat_message, sizeof(snapshot.chat_message), "%s", g_chat_message);
}

size_t CopyChatTranscript(ChatTranscriptEntry* destination, size_t capacity) {
    if (destination == nullptr || capacity == 0) return 0;
    std::lock_guard<std::mutex> lock(g_content_mutex);
    const size_t count = std::min(g_chat_history_count, capacity);
    const size_t first = (g_chat_history_next + chatai::display::kChatTranscriptCapacity -
                          g_chat_history_count) % chatai::display::kChatTranscriptCapacity;
    const size_t skip = g_chat_history_count - count;
    for (size_t i = 0; i < count; ++i) {
        destination[i] = g_chat_history[(first + skip + i) %
                                        chatai::display::kChatTranscriptCapacity];
    }
    return count;
}

void ClearChatTranscript() {
    std::lock_guard<std::mutex> lock(g_content_mutex);
    g_chat_history = {};
    g_chat_history_count = 0;
    g_chat_history_next = 0;
}

void SetBluetoothStatus(BluetoothStatus status, size_t device_count, int error_code) {
    g_bluetooth_device_count.store(device_count);
    g_bluetooth_error.store(error_code);
    g_bluetooth_status.store(static_cast<uint8_t>(status));
    int64_t duration_us = 0;
    switch (status) {
        case BluetoothStatus::kInitializing:
        case BluetoothStatus::kReady:
            duration_us = 3000000;
            break;
        case BluetoothStatus::kComplete:
        case BluetoothStatus::kError:
            duration_us = 5000000;
            break;
        case BluetoothStatus::kScanning:
            duration_us = 0;
            break;
        case BluetoothStatus::kIdle:
            break;
    }
    g_bluetooth_status_until_us.store(
        duration_us > 0 ? esp_timer_get_time() + duration_us : 0);
}

// Notifications no longer replace the real-time device-state screen.
void Notify(const char* title, const char* message, uint32_t duration_ms) {
    (void)title;
    (void)message;
    (void)duration_ms;
}

}  // namespace chatai::display
