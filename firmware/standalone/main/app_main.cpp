#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <nvs_flash.h>

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

    return true;
}
}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "ChatAI TPIOS standalone firmware starting");

    if (!InitializePlatform()) {
        ESP_LOGE(kTag, "Platform initialization stopped; no legacy fallback is loaded");
        return;
    }

    ESP_LOGI(kTag, "ESP-IDF platform foundation ready");
    ESP_LOGI(kTag, "Standalone mode: no XiaoZhi application or cloud components");
}
