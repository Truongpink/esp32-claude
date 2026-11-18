#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <nvs_flash.h>

#include "network_manager.h"
#include "display_manager.h"
#include "audio_manager.h"
#include "gemini_live_client.h"
#include "gemini_settings.h"

namespace {
constexpr char kTag[] = "ChatAI";

bool InitializePlatform() {
    const esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result != ESP_OK) {
        // Do not erase NVS automatically: existing user settings must be preserved.
        ESP_LOGE(kTag, "NVS initialization failed: %s", esp_err_to_name(nvs_result));
        return false;
    }

    const esp_err_t migration_result = chatai::settings::MigrateLegacyGeminiApiKey();
    if (migration_result != ESP_OK) {
        ESP_LOGW(kTag, "Could not migrate existing Gemini API Key; it can be entered in Web Admin");
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
        ESP_LOGE(kTag, "Platform initialization failed");
        return;
    }

    const esp_err_t gemini_result = chatai::gemini::Initialize();
    if (gemini_result != ESP_OK) {
        ESP_LOGE(kTag, "Gemini Live initialization failed: %s; restarting for OTA rollback",
                 esp_err_to_name(gemini_result));
        esp_restart();
        return;
    }

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state;
    if (running != nullptr &&
        esp_ota_get_state_partition(running, &image_state) == ESP_OK &&
        image_state == ESP_OTA_IMG_PENDING_VERIFY) {
        const esp_err_t validation_result = esp_ota_mark_app_valid_cancel_rollback();
        if (validation_result != ESP_OK) {
            ESP_LOGE(kTag, "Could not confirm OTA image: %s",
                     esp_err_to_name(validation_result));
            esp_restart();
            return;
        }
        ESP_LOGI(kTag, "OTA firmware startup validated; rollback cancelled");
    }
#endif

    ESP_LOGI(kTag, "ESP-IDF platform, native Wi-Fi and OLED startup completed");
    ESP_LOGI(kTag, "Gemini Live standalone mode initialized");
}
