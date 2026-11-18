#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>

#include "network_manager.h"
#include "display_manager.h"
#include "audio_manager.h"

namespace {
constexpr char kTag[] = "ChatAI";

bool InitializePlatform() {
    const esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result != ESP_OK) {
        // Do not erase NVS automatically: existing user settings must be preserved.
        ESP_LOGE(kTag, "NVS initialization failed: %s", esp_err_to_name(nvs_result));
        return false;
    }

    const esp_err_t netif_result = esp_netif_init();
    if (netif_result != ESP_OK) {
        ESP_LOGE(kTag, "Network interface initialization failed: %s",
                 esp_err_to_name(netif_result));
        return false;
    }

    const esp_err_t event_result = esp_event_loop_create_default();
    if (event_result != ESP_OK) {
        ESP_LOGE(kTag, "Default event loop initialization failed: %s",
                 esp_err_to_name(event_result));
        return false;
    }

    const esp_err_t wifi_result = chatai::network::NetworkManager::Initialize();
    if (wifi_result != ESP_OK) {
        ESP_LOGE(kTag, "ChatAI Wi-Fi initialization failed: %s",
                 esp_err_to_name(wifi_result));
        return false;
    }

    const esp_err_t display_result = chatai::display::Initialize();
    if (display_result != ESP_OK) {
        ESP_LOGW(kTag, "OLED initialization skipped: %s",
                 esp_err_to_name(display_result));
    }

    const esp_err_t audio_result = chatai::audio::Initialize();
    if (audio_result != ESP_OK) {
        ESP_LOGW(kTag, "Standalone I2S audio unavailable: %s",
                 esp_err_to_name(audio_result));
    }

    return true;
}
}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "ChatAI TPIOS standalone firmware starting");

    if (!InitializePlatform()) {
        ESP_LOGE(kTag, "Platform initialization stopped; no legacy fallback is loaded");
        return;
    }

    ESP_LOGI(kTag, "ESP-IDF platform, native Wi-Fi and OLED startup completed");
    ESP_LOGI(kTag, "Standalone mode: no XiaoZhi application or cloud components");
}
