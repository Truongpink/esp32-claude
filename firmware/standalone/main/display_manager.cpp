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
#include "vietnamese_font.h"

namespace {
constexpr char kTag[] = "ChatAI-OLED";
constexpr gpio_num_t kSdaPin = GPIO_NUM_41;
constexpr gpio_num_t kSclPin = GPIO_NUM_42;
constexpr uint16_t kAddress = 0x3C;
constexpr int kWidth = 128;
constexpr size_t kPages = 8;
constexpr int kGlyphWidth = 8;
constexpr int kGlyphHeight = 10;
constexpr int kFooterY = 53;
constexpr TickType_t kScrollStepTicks = pdMS_TO_TICKS(60);

i2c_master_bus_handle_t g_bus = nullptr;
i2c_master_dev_handle_t g_display = nullptr;
bool g_initialized = false;
bool g_sntp_started = false;
bool g_time_synced = false;
std::atomic<bool> g_audio_ready{false};
std::atomic<uint8_t> g_bluetooth_status{
    static_cast<uint8_t>(chatai::display::BluetoothStatus::kIdle)};
std::atomic<size_t> g_bluetooth_device_count{0};
std::atomic<int> g_bluetooth_error{0};
std::atomic<int64_t> g_bluetooth_status_until_us{0};
std::array<uint8_t, kWidth * kPages> g_frame{};
char g_footer[96] = "CHATAI TPIOS - WIFI WAIT - SERVER WAIT";
int g_footer_x = -static_cast<int>(sizeof("CHATAI TPIOS - WIFI WAIT - SERVER WAIT") - 1) * kGlyphWidth;
TickType_t g_last_scroll_tick = 0;

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

const chatai::display::Glyph8x10* FindGlyph(uint32_t codepoint) {
    const auto begin = std::begin(chatai::display::kVietnameseFont);
    const auto end = std::end(chatai::display::kVietnameseFont);
    const auto found = std::lower_bound(
        begin, end, codepoint,
        [](const chatai::display::Glyph8x10& glyph, uint32_t value) {
            return glyph.codepoint < value;
        });
    return found != end && found->codepoint == codepoint ? &*found : nullptr;
}

// Every character occupies the same 8x10 cell; clipping also supports the
// ticker entering from the left edge and moving toward the right.
void DrawText(int x, int y, std::string_view text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const uint32_t codepoint = DecodeUtf8(text, offset);
        const auto* glyph = FindGlyph(codepoint);
        if (glyph != nullptr) {
            int min_column = kGlyphWidth;
            int max_column = -1;
            for (int row = 0; row < kGlyphHeight; ++row) {
                for (int column = 0; column < kGlyphWidth; ++column) {
                    if ((glyph->rows[row] & (1U << (7 - column))) != 0) {
                        min_column = std::min(min_column, column);
                        max_column = std::max(max_column, column);
                    }
                }
            }
            if (max_column >= min_column) {
                const int glyph_width = max_column - min_column + 1;
                const int glyph_offset = (kGlyphWidth - glyph_width) / 2;
                for (int row = 0; row < kGlyphHeight; ++row) {
                    for (int column = min_column; column <= max_column; ++column) {
                        if ((glyph->rows[row] & (1U << (7 - column))) == 0) continue;
                        const int pixel_x = x + glyph_offset + column - min_column;
                        const int pixel_y = y + row;
                        if (pixel_x >= 0 && pixel_x < kWidth &&
                            pixel_y >= 0 && pixel_y < 64) {
                            g_frame[(pixel_y / 8) * kWidth + pixel_x] |=
                                static_cast<uint8_t>(1U << (pixel_y % 8));
                        }
                    }
                }
            }
        }
        x += kGlyphWidth;
    }
}

int TextWidth(std::string_view text) {
    return static_cast<int>(text.size()) * kGlyphWidth;
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

    if (!g_sntp_started) {
        // POSIX TZ offsets use the opposite sign: UTC-7 means Vietnam (UTC+7).
        setenv("TZ", "UTC-7", 1);
        tzset();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        g_sntp_started = true;
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
    if (std::strcmp(next, g_footer) == 0) return;
    std::snprintf(g_footer, sizeof(g_footer), "%s", next);
    g_footer_x = -TextWidth(g_footer);
}

const char* StatusText(chatai::xiaozhi::RuntimeState state) {
    using chatai::xiaozhi::RuntimeState;
    switch (state) {
        case RuntimeState::kConnecting: return "DANG KET NOI";
        case RuntimeState::kWaiting: return "DANG CHO";
        case RuntimeState::kListening: return "DANG NGHE";
        case RuntimeState::kProcessing: return "DANG XU LY";
        case RuntimeState::kSpeaking: return "DANG NOI";
        case RuntimeState::kError: return "LOI MICROPHONE";
    }
    return "DANG KET NOI";
}

esp_err_t FlushFrame() {
    for (uint8_t page = 0; page < kPages; ++page) {
        const uint8_t position[] = {
            0x00, static_cast<uint8_t>(0xB0 | page), 0x00, 0x10
        };
        esp_err_t result = i2c_master_transmit(g_display, position, sizeof(position), 100);
        if (result != ESP_OK) return result;
        uint8_t packet[kWidth + 1]{};
        packet[0] = 0x40;
        std::memcpy(packet + 1, g_frame.data() + page * kWidth, kWidth);
        result = i2c_master_transmit(g_display, packet, sizeof(packet), 250);
        if (result != ESP_OK) return result;
    }
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
        const char* status = !g_audio_ready.load()
            ? "LOI AM THANH"
            : StatusText(chatai::xiaozhi::GetRuntimeState());
        DrawText((kWidth - TextWidth(status)) / 2, 23, status);

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

    const esp_err_t result = FlushFrame();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "OLED refresh failed: %s", esp_err_to_name(result));
    }
}

void DisplayTask(void*) {
    while (true) {
        RenderStatus();
        vTaskDelay(pdMS_TO_TICKS(60));
    }
}
}  // namespace

namespace chatai::display {

esp_err_t Initialize() {
    if (g_initialized) return ESP_OK;

    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = kSdaPin;
    bus_config.scl_io_num = kSclPin;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    esp_err_t result = i2c_new_master_bus(&bus_config, &g_bus);
    if (result != ESP_OK) return result;

    i2c_device_config_t device_config{};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = kAddress;
    device_config.scl_speed_hz = 400000;
    result = i2c_master_bus_add_device(g_bus, &device_config, &g_display);
    if (result != ESP_OK) return result;

    const uint8_t initialization[] = {
        0x00, 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00,
        0x40, 0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8, 0xDA,
        0x12, 0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA6, 0xAF
    };
    result = i2c_master_transmit(g_display, initialization, sizeof(initialization), 250);
    if (result != ESP_OK) return result;

    g_initialized = true;
    g_last_scroll_tick = xTaskGetTickCount();
    if (xTaskCreate(DisplayTask, "chatai_oled", 4096, nullptr, 3, nullptr) != pdPASS) {
        g_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "Real-time OLED status initialized on SDA=%d SCL=%d",
             kSdaPin, kSclPin);
    return ESP_OK;
}

void SetAudioReady(bool ready) {
    g_audio_ready.store(ready);
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
