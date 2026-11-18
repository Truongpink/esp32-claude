#include "network_manager.h"

#include <atomic>
#include <cstring>
#include <lwip/ip4_addr.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs.h>

namespace {
constexpr char kTag[] = "ChatAI-WiFi";
constexpr char kNvsNamespace[] = "chatai_net";
constexpr char kSsidKey[] = "ssid";
constexpr char kPasswordKey[] = "password";
constexpr uint8_t kMaxReconnectAttempts = 5;

esp_netif_t* g_sta_netif = nullptr;
esp_event_handler_instance_t g_wifi_handler{};
esp_event_handler_instance_t g_ip_handler{};
bool g_initialized = false;
std::atomic<bool> g_has_credentials{false};
std::atomic<uint8_t> g_reconnect_attempts{0};
std::atomic<bool> g_connected{false};

bool LoadCredentials(std::string& ssid, std::string& password) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    size_t ssid_size = 0;
    size_t password_size = 0;
    const esp_err_t ssid_result = nvs_get_str(handle, kSsidKey, nullptr, &ssid_size);
    const esp_err_t password_result =
        nvs_get_str(handle, kPasswordKey, nullptr, &password_size);

    if (ssid_result != ESP_OK || password_result != ESP_OK ||
        ssid_size <= 1 || ssid_size > 33 || password_size == 0 || password_size > 65) {
        nvs_close(handle);
        return false;
    }

    ssid.resize(ssid_size);
    password.resize(password_size);
    const esp_err_t read_ssid =
        nvs_get_str(handle, kSsidKey, ssid.data(), &ssid_size);
    esp_err_t read_password = ESP_OK;
    if (password_size > 0) {
        read_password = nvs_get_str(handle, kPasswordKey, password.data(), &password_size);
    }
    nvs_close(handle);
    if (read_ssid == ESP_OK && read_password == ESP_OK) {
        ssid.resize(ssid_size - 1);
        password.resize(password_size - 1);
    }

    return read_ssid == ESP_OK && read_password == ESP_OK && !ssid.empty();
}

esp_err_t SaveCredentials(const std::string& ssid, const std::string& password) {
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;

    result = nvs_set_str(handle, kSsidKey, ssid.c_str());
    if (result == ESP_OK) result = nvs_set_str(handle, kPasswordKey, password.c_str());
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

void EventHandler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (g_has_credentials) {
            g_reconnect_attempts = 0;
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_connected.store(false);
        if (g_has_credentials && g_reconnect_attempts < kMaxReconnectAttempts) {
            ++g_reconnect_attempts;
            ESP_LOGW(kTag, "Wi-Fi disconnected; reconnect attempt %u/%u",
                     static_cast<unsigned>(g_reconnect_attempts),
                     static_cast<unsigned>(kMaxReconnectAttempts));
            esp_wifi_connect();
        } else {
            ESP_LOGW(kTag, "Wi-Fi is disconnected");
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(event_data);
        g_reconnect_attempts = 0;
        g_connected.store(true);
        ESP_LOGI(kTag, "Connected; IPv4 address: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}
}  // namespace

namespace chatai::network {

esp_err_t NetworkManager::Initialize() {
    if (g_initialized) return ESP_OK;

    std::string ssid;
    std::string password;
    g_has_credentials = LoadCredentials(ssid, password);

    g_sta_netif = esp_netif_create_default_wifi_sta();
    if (g_sta_netif == nullptr) return ESP_FAIL;

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t result = esp_wifi_init(&wifi_init_config);
    if (result != ESP_OK) return result;

    result = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &EventHandler, nullptr, &g_wifi_handler);
    if (result != ESP_OK) return result;

    result = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &EventHandler, nullptr, &g_ip_handler);
    if (result != ESP_OK) return result;

    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK) return result;

    if (g_has_credentials) {
        wifi_config_t config{};
        std::memcpy(config.sta.ssid, ssid.data(), ssid.size());
        std::memcpy(config.sta.password, password.data(), password.size());
        config.sta.threshold.authmode =
            password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        config.sta.pmf_cfg.capable = true;
        config.sta.pmf_cfg.required = false;

        result = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (result != ESP_OK) return result;
    }

    result = esp_wifi_start();
    if (result != ESP_OK) return result;

    g_initialized = true;
    ESP_LOGI(kTag, g_has_credentials
        ? "Wi-Fi station initialized; saved network will be connected"
        : "Wi-Fi station initialized; no saved network credentials");
    return ESP_OK;
}

esp_err_t NetworkManager::Connect(
    const std::string& ssid, const std::string& password) {
    if (!g_initialized) return ESP_ERR_INVALID_STATE;
    if (ssid.empty() || ssid.size() > 32 || password.size() > 64) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t config{};
    std::memcpy(config.sta.ssid, ssid.data(), ssid.size());
    std::memcpy(config.sta.password, password.data(), password.size());
    config.sta.threshold.authmode =
        password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    g_has_credentials.store(false);
    esp_wifi_disconnect();
    esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result != ESP_OK) return result;

    result = SaveCredentials(ssid, password);
    if (result != ESP_OK) return result;

    g_has_credentials.store(true);
    g_reconnect_attempts = 0;
    g_connected.store(false);
    return esp_wifi_connect();
}

bool NetworkManager::IsConnected() {
    return g_connected.load();
}

std::string NetworkManager::GetIpAddress() {
    if (!g_connected.load() || g_sta_netif == nullptr) return {};

    esp_netif_ip_info_t ip_info{};
    if (esp_netif_get_ip_info(g_sta_netif, &ip_info) != ESP_OK) return {};

    char address[16]{};
    std::snprintf(address, sizeof(address), IPSTR, IP2STR(&ip_info.ip));
    return address;
}

}  // namespace chatai::network
