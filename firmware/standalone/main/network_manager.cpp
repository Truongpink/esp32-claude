#include "network_manager.h"
#include "web_admin.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <lwip/ip4_addr.h>
#include <nvs.h>

namespace {
constexpr char kTag[] = "ChatAI-WiFi";
constexpr char kNvsNamespace[] = "chatai_net";
constexpr char kSsidKey[] = "ssid";
constexpr char kPasswordKey[] = "password";
constexpr uint8_t kMaxReconnectAttempts = 5;
constexpr size_t kMaxFormLength = 256;

esp_netif_t* g_sta_netif = nullptr;
esp_netif_t* g_ap_netif = nullptr;
httpd_handle_t g_http_server = nullptr;
esp_event_handler_instance_t g_wifi_handler{};
esp_event_handler_instance_t g_ip_handler{};
bool g_initialized = false;
std::atomic<bool> g_has_credentials{false};
std::atomic<uint8_t> g_reconnect_attempts{0};
std::atomic<bool> g_connected{false};
std::atomic<bool> g_provisioning_started{false};
std::string g_provisioning_ssid;
std::string g_form_token;

esp_err_t SaveCredentials(const std::string& ssid, const std::string& password);

bool ReadCredentials(const char* name_space, const char* ssid_key,
                     const char* password_key, std::string& ssid,
                     std::string& password) {
    nvs_handle_t handle;
    if (nvs_open(name_space, NVS_READONLY, &handle) != ESP_OK) return false;

    size_t ssid_size = 0;
    size_t password_size = 0;
    const esp_err_t ssid_result = nvs_get_str(handle, ssid_key, nullptr, &ssid_size);
    const esp_err_t password_result =
        nvs_get_str(handle, password_key, nullptr, &password_size);
    if (ssid_result != ESP_OK || password_result != ESP_OK ||
        ssid_size <= 1 || ssid_size > 33 || password_size == 0 || password_size > 65) {
        nvs_close(handle);
        return false;
    }

    ssid.resize(ssid_size);
    password.resize(password_size);
    const esp_err_t read_ssid = nvs_get_str(handle, ssid_key, ssid.data(), &ssid_size);
    const esp_err_t read_password =
        nvs_get_str(handle, password_key, password.data(), &password_size);
    nvs_close(handle);

    if (read_ssid == ESP_OK && read_password == ESP_OK) {
        ssid.resize(ssid_size - 1);
        password.resize(password_size - 1);
    }
    return read_ssid == ESP_OK && read_password == ESP_OK && !ssid.empty();
}

bool LoadCredentials(std::string& ssid, std::string& password) {
    if (ReadCredentials(kNvsNamespace, kSsidKey, kPasswordKey, ssid, password)) {
        return true;
    }

    // Xiaozhi stores saved networks in namespace "wifi".
    for (unsigned index = 0; index <= 9; ++index) {
        const std::string suffix = index == 0 ? "" : std::to_string(index);
        const std::string ssid_key = "ssid" + suffix;
        const std::string password_key = "password" + suffix;
        if (!ReadCredentials("wifi", ssid_key.c_str(), password_key.c_str(),
                             ssid, password)) {
            continue;
        }

        // Keep the original Xiaozhi namespace intact; migrate a copy for ChatAI.
        SaveCredentials(ssid, password);
        return true;
    }
    return false;
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

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool UrlDecode(const std::string& source, std::string& decoded) {
    decoded.clear();
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '+') {
            decoded.push_back(' ');
        } else if (source[i] == '%') {
            if (i + 2 >= source.size()) return false;
            const int high = HexValue(source[i + 1]);
            const int low = HexValue(source[i + 2]);
            if (high < 0 || low < 0) return false;
            const char value = static_cast<char>((high << 4) | low);
            if (value == '\0') return false;
            decoded.push_back(value);
            i += 2;
        } else {
            decoded.push_back(source[i]);
        }
    }
    return true;
}

bool GetFormValue(const std::string& body, const std::string& wanted,
                  std::string& value) {
    size_t start = 0;
    bool found = false;
    while (start <= body.size()) {
        const size_t end = body.find('&', start);
        const size_t pair_end = end == std::string::npos ? body.size() : end;
        const size_t equals = body.find('=', start);
        if (equals == std::string::npos || equals > pair_end) return false;

        std::string key;
        std::string decoded_value;
        if (!UrlDecode(body.substr(start, equals - start), key) ||
            !UrlDecode(body.substr(equals + 1, pair_end - equals - 1), decoded_value)) {
            return false;
        }
        if (key == wanted) {
            if (found) return false;
            value = std::move(decoded_value);
            found = true;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return found;
}

esp_err_t ConnectFromForm(const std::string& body) {
    std::string ssid, password, token;
    if (!GetFormValue(body, "ssid", ssid) ||
        !GetFormValue(body, "password", password) ||
        !GetFormValue(body, "token", token) || token != g_form_token ||
        ssid.empty() || ssid.size() > 32 || password.size() > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    return chatai::network::NetworkManager::Connect(ssid, password);
}

esp_err_t RootHandler(httpd_req_t* request) {
    if (!g_provisioning_started.load()) {
        httpd_resp_set_status(request, "302 Found");
        httpd_resp_set_hdr(request, "Location", "/admin");
        return httpd_resp_send(request, nullptr, 0);
    }
    const std::string page =
        "<!doctype html><html lang=\"vi\"><meta name=\"viewport\" "
        "content=\"width=device-width,initial-scale=1\"><meta charset=\"utf-8\">"
        "<title>ChatAI TPIOS - Wi-Fi</title><body style=\"font:16px sans-serif;"
        "max-width:480px;margin:40px auto;padding:0 18px\">"
        "<h2>Thiết lập Wi-Fi ChatAI TPIOS</h2>"
        "<p>Nhập tên và mật khẩu mạng Wi-Fi nhà anh.</p>"
        "<form method=\"post\" action=\"/wifi\">"
        "<input type=\"hidden\" name=\"token\" value=\"" + g_form_token + "\">"
        "<label>Tên Wi-Fi (SSID)</label><br><input name=\"ssid\" maxlength=\"32\" "
        "required style=\"box-sizing:border-box;width:100%;padding:12px;margin:8px 0\">"
        "<label>Mật khẩu</label><br><input name=\"password\" type=\"password\" "
        "maxlength=\"64\" style=\"box-sizing:border-box;width:100%;padding:12px;"
        "margin:8px 0\"><button style=\"padding:12px 20px\">Lưu và kết nối</button>"
        "</form><p><a href=\"/admin\">Mở Web Admin ChatAI</a></p></body></html>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, page.c_str(), HTTPD_RESP_USE_STRLEN);
}

esp_err_t WifiPostHandler(httpd_req_t* request) {
    if (request->content_len == 0 || request->content_len > kMaxFormLength) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form size");
        return ESP_FAIL;
    }

    std::string body(request->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int count = httpd_req_recv(
            request, body.data() + received, body.size() - received);
        if (count <= 0) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Incomplete form");
            return ESP_FAIL;
        }
        received += static_cast<size_t>(count);
    }

    if (ConnectFromForm(body) != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi settings");
        return ESP_FAIL;
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_sendstr(request,
        "<!doctype html><html lang=\"vi\"><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<body style=\"font:16px sans-serif;padding:24px\"><h2>Da luu Wi-Fi</h2>"
        "<p>ChatAI dang ket noi. Hay cho thiet bi khoi dong ket noi mang.</p></body></html>");
}

esp_err_t StartHttpServer() {
    if (g_http_server != nullptr) return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    esp_err_t result = httpd_start(&g_http_server, &config);
    if (result != ESP_OK) return result;

    httpd_uri_t root_uri{};
    root_uri.uri = "/";
    root_uri.method = HTTP_GET;
    root_uri.handler = RootHandler;

    httpd_uri_t wifi_uri{};
    wifi_uri.uri = "/wifi";
    wifi_uri.method = HTTP_POST;
    wifi_uri.handler = WifiPostHandler;

    result = httpd_register_uri_handler(g_http_server, &root_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(g_http_server, &wifi_uri);
    if (result == ESP_OK) result = chatai::web_admin::RegisterRoutes(g_http_server, g_form_token);
    if (result != ESP_OK) {
        httpd_stop(g_http_server);
        g_http_server = nullptr;
    }
    return result;
}

esp_err_t StartProvisioning() {
    bool expected = false;
    if (!g_provisioning_started.compare_exchange_strong(expected, true)) return ESP_OK;

    if (g_ap_netif == nullptr) {
        g_ap_netif = esp_netif_create_default_wifi_ap();
        if (g_ap_netif == nullptr) {
            g_provisioning_started.store(false);
            return ESP_FAIL;
        }
    }

    uint8_t mac[6]{};
    esp_err_t result = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (result != ESP_OK) {
        g_provisioning_started.store(false);
        return result;
    }

    char ssid[33]{};
    std::snprintf(ssid, sizeof(ssid), "ChatAI-%02X%02X%02X", mac[3], mac[4], mac[5]);
    g_provisioning_ssid = ssid;

    wifi_config_t ap_config{};
    std::memcpy(ap_config.ap.ssid, g_provisioning_ssid.data(),
                g_provisioning_ssid.size());
    ap_config.ap.ssid_len = g_provisioning_ssid.size();
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 2;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.pmf_cfg.required = false;

    result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result == ESP_OK) result = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (result != ESP_OK) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        g_provisioning_started.store(false);
        return result;
    }

    ESP_LOGI(kTag, "Open Wi-Fi setup AP started");
    return ESP_OK;
}

esp_err_t StopProvisioning() {
    if (!g_provisioning_started.exchange(false)) return ESP_OK;
    const esp_err_t result = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(kTag, "Wi-Fi setup AP stopped after station connection");
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
            if (g_initialized) {
                const esp_err_t result = StartProvisioning();
                if (result != ESP_OK) {
                    ESP_LOGE(kTag, "Could not start Wi-Fi provisioning: %s",
                             esp_err_to_name(result));
                }
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(event_data);
        g_reconnect_attempts = 0;
        g_connected.store(true);
        ESP_LOGI(kTag, "Connected; IPv4 address: " IPSTR, IP2STR(&event->ip_info.ip));
        if (g_provisioning_started.load()) StopProvisioning();
    }
}
}  // namespace

namespace chatai::network {

esp_err_t NetworkManager::Initialize() {
    if (g_initialized) return ESP_OK;

    std::string ssid, password;
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

    char token[17]{};
    std::snprintf(token, sizeof(token), "%08lx%08lx",
                  static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()));
    g_form_token = token;

    result = StartHttpServer();
    if (result != ESP_OK) return result;

    if (!g_has_credentials) {
        result = StartProvisioning();
        if (result != ESP_OK) return result;
    }

    if (g_has_credentials) {
        ESP_LOGI(kTag, "Wi-Fi station initialized; saved network will be connected");
    } else {
        ESP_LOGI(kTag, "No saved Wi-Fi network; open setup AP started");
    }
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

bool NetworkManager::IsProvisioningActive() {
    return g_provisioning_started.load();
}

std::string NetworkManager::GetProvisioningSsid() {
    return g_provisioning_ssid;
}

}  // namespace chatai::network
