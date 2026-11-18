#include "display_manager.h"
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "network_manager.h"

namespace {
constexpr char kTag[] = "ChatAI-OLED";
constexpr gpio_num_t kSdaPin = GPIO_NUM_41;
constexpr gpio_num_t kSclPin = GPIO_NUM_42;
constexpr uint16_t kAddress = 0x3C;
constexpr size_t kWidth = 128;
constexpr size_t kPages = 8;
i2c_master_bus_handle_t g_bus = nullptr;
i2c_master_dev_handle_t g_display = nullptr;
bool g_initialized = false;
std::array<uint8_t, kWidth * kPages> g_frame{};

std::array<uint8_t, 5> Glyph(char value) {
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(value)))) {
        case 'A': return {0x7E,0x11,0x11,0x11,0x7E};
        case 'B': return {0x7F,0x49,0x49,0x49,0x36};
        case 'C': return {0x3E,0x41,0x41,0x41,0x22};
        case 'D': return {0x7F,0x41,0x41,0x22,0x1C};
        case 'E': return {0x7F,0x49,0x49,0x49,0x41};
        case 'F': return {0x7F,0x09,0x09,0x09,0x01};
        case 'G': return {0x3E,0x41,0x49,0x49,0x7A};
        case 'H': return {0x7F,0x08,0x08,0x08,0x7F};
        case 'I': return {0x00,0x41,0x7F,0x41,0x00};
        case 'J': return {0x20,0x40,0x41,0x3F,0x01};
        case 'K': return {0x7F,0x08,0x14,0x22,0x41};
        case 'L': return {0x7F,0x40,0x40,0x40,0x40};
        case 'M': return {0x7F,0x02,0x0C,0x02,0x7F};
        case 'N': return {0x7F,0x04,0x08,0x10,0x7F};
        case 'O': return {0x3E,0x41,0x41,0x41,0x3E};
        case 'P': return {0x7F,0x09,0x09,0x09,0x06};
        case 'Q': return {0x3E,0x41,0x51,0x21,0x5E};
        case 'R': return {0x7F,0x09,0x19,0x29,0x46};
        case 'S': return {0x46,0x49,0x49,0x49,0x31};
        case 'T': return {0x01,0x01,0x7F,0x01,0x01};
        case 'U': return {0x3F,0x40,0x40,0x40,0x3F};
        case 'V': return {0x1F,0x20,0x40,0x20,0x1F};
        case 'W': return {0x3F,0x40,0x38,0x40,0x3F};
        case 'X': return {0x63,0x14,0x08,0x14,0x63};
        case 'Y': return {0x07,0x08,0x70,0x08,0x07};
        case 'Z': return {0x61,0x51,0x49,0x45,0x43};
        case '0': return {0x3E,0x51,0x49,0x45,0x3E};
        case '1': return {0x00,0x42,0x7F,0x40,0x00};
        case '2': return {0x62,0x51,0x49,0x49,0x46};
        case '3': return {0x22,0x41,0x49,0x49,0x36};
        case '4': return {0x18,0x14,0x12,0x7F,0x10};
        case '5': return {0x2F,0x49,0x49,0x49,0x31};
        case '6': return {0x3E,0x49,0x49,0x49,0x32};
        case '7': return {0x01,0x71,0x09,0x05,0x03};
        case '8': return {0x36,0x49,0x49,0x49,0x36};
        case '9': return {0x26,0x49,0x49,0x49,0x3E};
        case ':': return {0x00,0x36,0x36,0x00,0x00};
        case '.': return {0x00,0x60,0x60,0x00,0x00};
        case '-': return {0x08,0x08,0x08,0x08,0x08};
        case '/': return {0x20,0x10,0x08,0x04,0x02};
        default: return {0x00,0x00,0x00,0x00,0x00};
    }
}

void DrawText(size_t x, size_t y, const std::string& text) {
    if (y >= 64) return;
    for (char value : text) {
        if (x + 5 >= kWidth) break;
        const auto glyph = Glyph(value);
        for (size_t column = 0; column < glyph.size(); ++column) {
            g_frame[(y / 8) * kWidth + x + column] = glyph[column];
        }
        x += 6;
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
    DrawText(0, 0, "CHATAI TPIOS");
    if (chatai::network::NetworkManager::IsProvisioningActive()) {
        DrawText(0, 16, "WIFI SETUP");
        DrawText(0, 24, "SSID:" +
            chatai::network::NetworkManager::GetProvisioningSsid());
        DrawText(0, 40, "OPEN WIFI");
        DrawText(0, 52, "192.168.4.1");
    } else if (chatai::network::NetworkManager::IsConnected()) {
        DrawText(0, 16, "WIFI CONNECTED");
        DrawText(0, 32, "IP:" +
            chatai::network::NetworkManager::GetIpAddress());
        DrawText(0, 48, "NETWORK READY");
    } else {
        DrawText(0, 16, "WIFI CONNECTING");
        DrawText(0, 32, "PLEASE WAIT");
    }
    const esp_err_t result = FlushFrame();
    if (result != ESP_OK) ESP_LOGW(kTag, "OLED refresh failed: %s", esp_err_to_name(result));
}

void DisplayTask(void*) {
    while (true) {
        RenderStatus();
        vTaskDelay(pdMS_TO_TICKS(1000));
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
    if (xTaskCreate(DisplayTask, "chatai_oled", 4096, nullptr, 3, nullptr) != pdPASS) {
        g_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "Standalone SSD1306 initialized on SDA=%d SCL=%d", kSdaPin, kSclPin);
    return ESP_OK;
}
}  // namespace chatai::display
