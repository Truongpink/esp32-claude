#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mdns.h>
#include <nvs_flash.h>

#include "network_manager.h"
#include "display_manager.h"
#include "audio_manager.h"
#include "gemini_live_client.h"
#include "gemini_settings.h"
#include "firmware_info.h"
#include "button_manager.h"
#include "diagnostic_manager.h"

namespace {
constexpr char kTag[] = "ChatAI";

esp_err_t InitializeMdns() {
    esp_err_t result = mdns_init();
    if (result != ESP_OK) {
        return result;
    }

    result = mdns_hostname_set("chatai");
    if (result == ESP_OK) {
        result = mdns_instance_name_set("ChatAI TPIOS");
    }
    if (result != ESP_OK) {
        mdns_free();
        return result;
    }

    const esp_err_t service_result = mdns_service_add(
        "ChatAI TPIOS Web Admin", "_http", "_tcp", 80, nullptr, 0);
    if (service_result != ESP_OK) {
        ESP_LOGW(kTag, "Could not advertise HTTP service over mDNS: %s",
                 esp_err_to_name(service_result));
    }

    ESP_LOGI(kTag, "Web Admin local address: http://chatai.local/");
    return ESP_OK;
}

const char* ResetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN: return "UNKNOWN";
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXTERNAL";
        case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WATCHDOG";
        case ESP_RST_TASK_WDT: return "TASK_WATCHDOG";
        case ESP_RST_WDT: return "WATCHDOG";
        case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "OTHER";
    }
}

void ReportBootDiagnostics() {
    const esp_reset_reason_t reason = esp_reset_reason();
    const size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    char detail[160]{};
    std::snprintf(detail, sizeof(detail),
                  "boot_id=%08lx reset_reason=%s(%d) free_heap=%lu min_free_heap=%lu",
                  static_cast<unsigned long>(esp_random()), ResetReasonName(reason),
                  static_cast<int>(reason), static_cast<unsigned long>(free_heap),
                  static_cast<unsigned long>(min_free_heap));
    chatai::diagnostics::Add("BOOT", "INFO", detail);
    chatai::diagnostics::RestorePreviousCrashContext();
}

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

    const esp_err_t mdns_result = InitializeMdns();
    if (mdns_result != ESP_OK) {
        ESP_LOGW(kTag, "mDNS unavailable; Web Admin remains accessible by IP: %s",
                 esp_err_to_name(mdns_result));
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
    chatai::display::SetAudioReady(audio_result == ESP_OK);
    if (audio_result != ESP_OK) {
        ESP_LOGW(kTag, "Standalone I2S audio unavailable: %s",
                 esp_err_to_name(audio_result));
    }

    return true;
}

void RestartIfOtaImageIsPending() {
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state;
    if (running != nullptr &&
        esp_ota_get_state_partition(running, &image_state) == ESP_OK &&
        image_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE(kTag, "New OTA image failed startup; restarting for bootloader rollback");
        esp_restart();
    }
#endif
}
}  // namespace

extern "C" void app_main(void) {
    ReportBootDiagnostics();
    ESP_LOGI(kTag, "ChatAI TPIOS standalone firmware starting");
    ESP_LOGI(kTag, "Firmware Build #%lu",
             static_cast<unsigned long>(chatai::firmware::kBuildNumber));

    if (!InitializePlatform()) {
        ESP_LOGE(kTag, "Platform initialization failed");
        RestartIfOtaImageIsPending();
        return;
    }

    const esp_err_t buttons_result = chatai::buttons::Initialize();
    if (buttons_result != ESP_OK) {
        ESP_LOGW(kTag, "Hardware button initialization failed: %s", esp_err_to_name(buttons_result));
    }

    const esp_err_t gemini_result = chatai::gemini::Initialize();
    if (gemini_result != ESP_OK) {
        ESP_LOGE(kTag, "Gemini Live initialization failed: %s",
                 esp_err_to_name(gemini_result));
        RestartIfOtaImageIsPending();
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
