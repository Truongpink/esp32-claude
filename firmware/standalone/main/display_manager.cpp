#include "display_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string_view>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "gemini_live_client.h"
#include "gemini_settings.h"
#include "network_manager.h"
#include "vietnamese_font.h"

namespace {
constexpr char kTag[] = "ChatAI-OLED";
constexpr gpio_num_t kSdaPin = GPIO_NUM_41;
constexpr gpio_num_t kSclPin = GPIO_NUM_42;
constexpr uint16_t kAddress = 0x3C;
constexpr size_t kWidth = 128;
constexpr size_t kPages = 8;
constexpr TickType_t kBootLogoDuration = pdMS_TO_TICKS(1800);

i2c_master_bus_handle_t g_bus = nullptr;
i2c_master_dev_handle_t g_display = nullptr;
bool g_initialized = false;
std::atomic<bool> g_audio_ready{false};
std::mutex g_notification_mutex;
char g_notification_title[24]{};
char g_notification_message[24]{};
TickType_t g_notification_until = 0;
TickType_t g_boot_screen_until = 0;
std::array<uint8_t, kWidth * kPages> g_frame{};

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
    return found != end && found->codepoint == codepoint ? found : nullptr;
}

void DrawText(size_t x, size_t y, std::string_view text) {
    size_t offset = 0;
    while (offset < text.size() && x + 8 <= kWidth && y + 10 <= 64) {
        const uint32_t codepoint = DecodeUtf8(text, offset);
        const auto* glyph = FindGlyph(codepoint);
        if (glyph != nullptr) {
            for (size_t row = 0; row < 10; ++row) {
                for (size_t column = 0; column < 8; ++column) {
                    if ((glyph->rows[row] & (1U << (7 - column))) == 0) continue;
                    const size_t pixel_x = x + column;
                    const size_t pixel_y = y + row;
                    g_frame[(pixel_y / 8) * kWidth + pixel_x] |=
                        static_cast<uint8_t>(1U << (pixel_y % 8));
                }
            }
        }
        x += 8;
    }
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
    DrawText(8, 8, "ChatAI TPIOS");

    const TickType_t now = xTaskGetTickCount();
    char notification_title[sizeof(g_notification_title)]{};
    char notification_message[sizeof(g_notification_message)]{};
    bool show_notification = false;
    {
        std::lock_guard<std::mutex> lock(g_notification_mutex);
        show_notification = static_cast<int32_t>(g_notification_until - now) > 0;
        if (show_notification) {
            std::snprintf(notification_title, sizeof(notification_title), "%s", g_notification_title);
            std::snprintf(notification_message, sizeof(notification_message), "%s", g_notification_message);
        }
    }

    if (static_cast<int32_t>(g_boot_screen_until - now) > 0) {
        DrawText(8, 30, "Đang khởi động");
    } else if (show_notification) {
        DrawText(0, 20, notification_title);
        DrawText(0, 32, notification_message);
    } else if (chatai::network::NetworkManager::IsProvisioningActive()) {
        DrawText(0, 26,
            chatai::network::NetworkManager::HasSavedCredentials()
                ? "Mất kết nối mạng" : "Chưa có Wi-Fi");
        DrawText(8, 44, "Mở 192.168.4.1");
    } else if (!chatai::network::NetworkManager::IsConnected()) {
        DrawText(0, 30, "Kết nối Wi-Fi");
    } else if (!chatai::network::NetworkManager::IsInternetAvailable()) {
        DrawText(8, 30, "Kiểm tra Internet");
    } else if (!chatai::settings::HasGeminiApiKey()) {
        DrawText(0, 26, "Thiếu Gemini Key");
        DrawText(8, 44, "Mở chatai.local");
    } else if (!g_audio_ready.load()) {
        DrawText(8, 30, "Lỗi âm thanh");
    } else if (chatai::gemini::HasError()) {
        DrawText(8, 30, "Lỗi Gemini");
    } else if (!chatai::gemini::IsReady()) {
        DrawText(8, 30, "Kết nối Gemini");
    } else if (chatai::gemini::IsSpeaking()) {
        DrawText(8, 30, "Đang trả lời");
    } else if (chatai::gemini::IsListening()) {
        DrawText(8, 30, "Đang nghe");
    } else {
        DrawText(0, 30, "Gemini sẵn sàng");
    }

    if (chatai::network::NetworkManager::IsConnected()) {
        const std::string ip = chatai::network::NetworkManager::GetIpAddress();
        DrawText(0, 42, "IP:");
        DrawText(0, 52, ip.empty() ? "Đang nhận IP" : ip);
    }

    const esp_err_t result = FlushFrame();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "OLED refresh failed: %s", esp_err_to_name(result));
    }
}

void DisplayTask(void*) {
    while (true) {
        RenderStatus();
        vTaskDelay(pdMS_TO_TICKS(250));
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

    // The current board configuration uses the SSD1306 command sequence.
    const uint8_t initialization[] = {
        0x00, 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00,
        0x40, 0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8, 0xDA,
        0x12, 0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA6, 0xAF
    };
    result = i2c_master_transmit(g_display, initialization, sizeof(initialization), 250);
    if (result != ESP_OK) return result;

    g_boot_screen_until = xTaskGetTickCount() + kBootLogoDuration;
    g_initialized = true;
    if (xTaskCreate(DisplayTask, "chatai_oled", 4096, nullptr, 3, nullptr) != pdPASS) {
        g_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "Vietnamese OLED initialized on SDA=%d SCL=%d", kSdaPin, kSclPin);
    return ESP_OK;
}

void SetAudioReady(bool ready) {
    g_audio_ready.store(ready);
}

void Notify(const char* title, const char* message, uint32_t duration_ms) {
    std::lock_guard<std::mutex> lock(g_notification_mutex);
    std::snprintf(g_notification_title, sizeof(g_notification_title), "%s",
                  title != nullptr ? title : "ChatAI");
    std::snprintf(g_notification_message, sizeof(g_notification_message), "%s",
                  message != nullptr ? message : "");
    g_notification_until = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
}

}  // namespace chatai::display
