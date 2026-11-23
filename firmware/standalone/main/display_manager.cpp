#include "display_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <iterator>
#include <string_view>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
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
std::atomic<bool> g_audio_ready{false};
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
    return found != end && found->codepoint == codepoint ? found : nullptr;
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

void UpdateFooter() {
    const bool wifi_ready = chatai::network::NetworkManager::IsConnected();
    const bool server_ready = chatai::xiaozhi::IsReady();
    char next[sizeof(g_footer)]{};
    std::snprintf(next, sizeof(next), "CHATAI TPIOS - WIFI %s - SERVER %s",
                  wifi_ready ? "OK" : "WAIT",
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

    const char* status = !g_audio_ready.load()
        ? "LOI AM THANH"
        : StatusText(chatai::xiaozhi::GetRuntimeState());
    DrawText((kWidth - TextWidth("CHATAI TPIOS")) / 2, 5, "CHATAI TPIOS");
    DrawText((kWidth - TextWidth(status)) / 2, 23, status);

    for (int x = 0; x < kWidth; x += 2) {
        const int y = 46;
        g_frame[(y / 8) * kWidth + x] |= static_cast<uint8_t>(1U << (y % 8));
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

// Notifications no longer replace the real-time device-state screen.
void Notify(const char* title, const char* message, uint32_t duration_ms) {
    (void)title;
    (void)message;
    (void)duration_ms;
}

}  // namespace chatai::display
