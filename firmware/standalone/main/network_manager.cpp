#include "network_manager.h"
#include "web_admin.h"
#include "firmware_info.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <sys/time.h>

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
constexpr char kFirmwareBuildKey[] = "fw_build";

std::string CurrentFirmwareBuildId() {
    char id[64]{};
    std::snprintf(id, sizeof(id), "%lu-%s-%s",
                  static_cast<unsigned long>(chatai::firmware::kBuildNumber),
                  __DATE__, __TIME__);
    return id;
}
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
std::atomic<bool> g_internet_available{false};
std::atomic<bool> g_provisioning_started{false};
std::string g_provisioning_ssid;
std::string g_form_token;
bool g_dns_started = false;

bool ProbeGeminiInternet() {
    if (g_sta_netif == nullptr) return false;

    esp_netif_ip_info_t ip_info{};
    if (esp_netif_get_ip_info(g_sta_netif, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0 || ip_info.gw.addr == 0) {
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const int lookup_result = getaddrinfo(
        "generativelanguage.googleapis.com", "443", &hints, &addresses);
    if (lookup_result != 0 || addresses == nullptr) return false;

    bool reachable = false;
    for (addrinfo* address = addresses; address != nullptr && !reachable;
         address = address->ai_next) {
        const int socket_fd = socket(address->ai_family, address->ai_socktype,
                                     address->ai_protocol);
        if (socket_fd < 0) continue;

        const int flags = fcntl(socket_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(socket_fd);
            continue;
        }

        const int connect_result = connect(
            socket_fd, address->ai_addr, address->ai_addrlen);
        if (connect_result == 0) {
            reachable = true;
        } else if (errno == EINPROGRESS) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(socket_fd, &write_set);
            timeval timeout{};
            timeout.tv_sec = 4;
            const int selected = select(
                socket_fd + 1, nullptr, &write_set, nullptr, &timeout);
            if (selected > 0) {
                int socket_error = 0;
                socklen_t error_length = sizeof(socket_error);
                reachable = getsockopt(
                    socket_fd, SOL_SOCKET, SO_ERROR,
                    &socket_error, &error_length) == 0 && socket_error == 0;
            }
        }
        close(socket_fd);
    }

    freeaddrinfo(addresses);
    return reachable;
}

void InternetCheckTask(void*) {
    while (true) {
        if (!g_connected.load()) {
            g_internet_available.store(false);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const bool available = ProbeGeminiInternet();
        const bool previous = g_internet_available.exchange(available);
        if (available != previous) {
            if (available) {
                ESP_LOGI(kTag, "Internet verified: Gemini DNS resolved and TCP port 443 reachable");
            } else {
                ESP_LOGW(kTag, "Internet unavailable: Gemini DNS or TCP port 443 check failed");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(available ? 10000 : 5000));
    }
}

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

esp_err_t EraseCredentialNamespace(const char* name_space);

bool LoadCredentials(std::string& ssid, std::string& password) {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        EraseCredentialNamespace("wifi");
        return false;
    }

    size_t build_id_size = 0;
    esp_err_t marker_result = nvs_get_str(handle, kFirmwareBuildKey, nullptr, &build_id_size);
    std::string saved_build_id;
    if (marker_result == ESP_OK && build_id_size > 1 && build_id_size <= 64) {
        saved_build_id.resize(build_id_size);
        marker_result = nvs_get_str(handle, kFirmwareBuildKey, saved_build_id.data(),
                                    &build_id_size);
        if (marker_result == ESP_OK) saved_build_id.resize(build_id_size - 1);
    }
    nvs_close(handle);
    if (marker_result != ESP_OK || saved_build_id != CurrentFirmwareBuildId()) {
        EraseCredentialNamespace(kNvsNamespace);
        EraseCredentialNamespace("wifi");
        ESP_LOGI(kTag, "Firmware build changed; saved Wi-Fi credentials require setup again");
        return false;
    }

    // Never reuse credentials left by the previous Xiaozhi firmware.
    EraseCredentialNamespace("wifi");
    return ReadCredentials(kNvsNamespace, kSsidKey, kPasswordKey, ssid, password);
}


esp_err_t EraseCredentialNamespace(const char* name_space) {
    nvs_handle_t handle;
    esp_err_t result = nvs_open(name_space, NVS_READWRITE, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    bool changed = false;
    for (unsigned index = 0; index <= 9; ++index) {
        const std::string suffix = index == 0 ? "" : std::to_string(index);
        const std::string ssid_key = "ssid" + suffix;
        const std::string password_key = "password" + suffix;
        for (const char* key : {ssid_key.c_str(), password_key.c_str()}) {
            result = nvs_erase_key(handle, key);
            if (result == ESP_OK) changed = true;
            else if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
            else {
                nvs_close(handle);
                return result;
            }
        }
    }
    if (std::strcmp(name_space, kNvsNamespace) == 0) {
        result = nvs_erase_key(handle, kFirmwareBuildKey);
        if (result == ESP_OK) changed = true;
        else if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
        else {
            nvs_close(handle);
            return result;
        }
    }
    if (changed) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

esp_err_t SaveCredentials(const std::string& ssid, const std::string& password) {
    nvs_handle_t handle;
    esp_err_t result = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_str(handle, kSsidKey, ssid.c_str());
    if (result == ESP_OK) result = nvs_set_str(handle, kPasswordKey, password.c_str());
    if (result == ESP_OK) {
        const std::string build_id = CurrentFirmwareBuildId();
        result = nvs_set_str(handle, kFirmwareBuildKey, build_id.c_str());
    }
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

std::string JsonEscape(const uint8_t* value, size_t length) {
    constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = value[i];
        if (c == '"' || c == '\\') {
            escaped.push_back('\\');
            escaped.push_back(static_cast<char>(c));
        } else if (c < 0x20) {
            escaped += "\\u00";
            escaped.push_back(hex[c >> 4]);
            escaped.push_back(hex[c & 0x0f]);
        } else {
            escaped.push_back(static_cast<char>(c));
        }
    }
    return escaped;
}

void DnsServerTask(void* argument) {
    const int socket_fd = static_cast<int>(reinterpret_cast<intptr_t>(argument));
    uint8_t query[512]{};
    uint8_t response[528]{};
    while (true) {
        sockaddr_in client{};
        socklen_t client_length = sizeof(client);
        const int received = recvfrom(socket_fd, query, sizeof(query), 0,
                                      reinterpret_cast<sockaddr*>(&client), &client_length);
        if (received < 17 || !g_provisioning_started.load()) continue;
        size_t question_end = 12;
        bool valid_name = false;
        while (question_end < static_cast<size_t>(received)) {
            const uint8_t label_length = query[question_end];
            if (label_length == 0) {
                ++question_end;
                valid_name = true;
                break;
            }
            if ((label_length & 0xc0) != 0 || label_length > 63 ||
                question_end + 1 + label_length >= static_cast<size_t>(received)) break;
            question_end += 1 + label_length;
        }
        if (!valid_name || question_end + 4 > static_cast<size_t>(received)) continue;
        const uint16_t query_type =
            (static_cast<uint16_t>(query[question_end]) << 8) | query[question_end + 1];
        const uint16_t query_class =
            (static_cast<uint16_t>(query[question_end + 2]) << 8) | query[question_end + 3];
        const bool answer_a = query_type == 1 && query_class == 1;
        const size_t question_size = question_end + 4;
        std::memcpy(response, query, question_size);
        response[2] = static_cast<uint8_t>(0x80 | (query[2] & 0x01));
        response[3] = 0x80;
        response[6] = 0;
        response[7] = answer_a ? 1 : 0;
        response[8] = response[9] = response[10] = response[11] = 0;
        size_t response_size = question_size;
        if (answer_a) {
            const uint8_t answer[] = {
                0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01,
                0x00, 0x00, 0x00, 0x3c, 0x00, 0x04,
                192, 168, 4, 1
            };
            std::memcpy(response + response_size, answer, sizeof(answer));
            response_size += sizeof(answer);
        }
        sendto(socket_fd, response, response_size, 0,
               reinterpret_cast<sockaddr*>(&client), client_length);
    }
}

esp_err_t StartDnsServer() {
    if (g_dns_started) return ESP_OK;
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) return ESP_FAIL;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(53);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(socket_fd);
        return ESP_FAIL;
    }
    timeval timeout{};
    timeout.tv_sec = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (xTaskCreate(DnsServerTask, "chatai_dns", 3072,
                    reinterpret_cast<void*>(static_cast<intptr_t>(socket_fd)),
                    3, nullptr) != pdPASS) {
        close(socket_fd);
        return ESP_ERR_NO_MEM;
    }
    g_dns_started = true;
    ESP_LOGI(kTag, "Captive portal DNS server listening on UDP port 53");
    return ESP_OK;
}

esp_err_t WifiScanHandler(httpd_req_t* request) {
    if (!g_provisioning_started.load()) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "{\"error\":\"Chế độ cấu hình Wi-Fi không hoạt động\"}");
    }
    wifi_scan_config_t scan_config{};
    scan_config.show_hidden = false;
    esp_err_t result = esp_wifi_scan_start(&scan_config, true);
    if (result != ESP_OK) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "{\"error\":\"Không thể quét Wi-Fi lúc này\"}");
    }
    uint16_t count = 0;
    result = esp_wifi_scan_get_ap_num(&count);
    if (result != ESP_OK) return ESP_FAIL;
    if (count > 40) count = 40;
    std::vector<wifi_ap_record_t> records(count);
    if (count > 0) {
        result = esp_wifi_scan_get_ap_records(&count, records.data());
        if (result != ESP_OK) return ESP_FAIL;
    }
    std::string body = "{\"networks\":[";
    for (uint16_t i = 0; i < count; ++i) {
        if (i != 0) body.push_back(',');
        const size_t length = strnlen(
            reinterpret_cast<const char*>(records[i].ssid), sizeof(records[i].ssid));
        body += "{\"ssid\":\"" + JsonEscape(records[i].ssid, length) +
                "\",\"rssi\":" + std::to_string(records[i].rssi) +
                ",\"auth\":" + std::to_string(records[i].authmode) + "}";
    }
    body += "]}";
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, body.c_str(), body.size());
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
        "<label>Chọn mạng Wi-Fi</label><br><select id=\"ssid\" name=\"ssid\" required "
        "style=\"box-sizing:border-box;width:100%;padding:12px;margin:8px 0\">"
        "<option value=\"\">Nhấn Quét Wi-Fi để tìm mạng</option></select>"
        "<button type=\"button\" id=\"scanWifi\" style=\"padding:12px 20px\">Quét Wi-Fi</button>"
        "<label>Mật khẩu</label><br><input name=\"password\" type=\"password\" "
        "maxlength=\"64\" style=\"box-sizing:border-box;width:100%;padding:12px;"
        "margin:8px 0\"><button style=\"padding:12px 20px\">Lưu và kết nối</button>"
        "</form><p id=\"scanStatus\">Đang tìm mạng Wi-Fi...</p>"
        "<p><a href=\"/admin\">Mở Web Admin ChatAI</a></p>"
        "<script>const list=document.getElementById('ssid');"
        "const status=document.getElementById('scanStatus');"
        "async function scanWifi(){status.textContent='Đang quét Wi-Fi...';"
        "try{const r=await fetch('/api/wifi/scan',{cache:'no-store'});"
        "if(!r.ok)throw new Error('Không quét được Wi-Fi');const d=await r.json();"
        "const old=list.value;list.replaceChildren();"
        "for(const n of d.networks){const o=document.createElement('option');"
        "o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';list.appendChild(o);}"
        "if(!d.networks.length){const o=document.createElement('option');"
        "o.value='';o.textContent='Không tìm thấy mạng Wi-Fi';list.appendChild(o);}"
        "else if(d.networks.some(n=>n.ssid===old))list.value=old;"
        "status.textContent='Tìm thấy '+d.networks.length+' mạng Wi-Fi.';"
        "}catch(e){status.textContent=e.message;}}"
        "document.getElementById('scanWifi').addEventListener('click',scanWifi);"
        "scanWifi();</script></body></html>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, page.c_str(), HTTPD_RESP_USE_STRLEN);
}

esp_err_t WifiPostHandler(httpd_req_t* request) {
    if (request->content_len == 0 || request->content_len > kMaxFormLength) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Kích thước biểu mẫu không hợp lệ");
        return ESP_FAIL;
    }

    std::string body(request->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int count = httpd_req_recv(
            request, body.data() + received, body.size() - received);
        if (count <= 0) {
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Dữ liệu biểu mẫu chưa đầy đủ");
            return ESP_FAIL;
        }
        received += static_cast<size_t>(count);
    }

    if (ConnectFromForm(body) != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Thông tin Wi-Fi không hợp lệ");
        return ESP_FAIL;
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_sendstr(request,
        "<!doctype html><html lang=\"vi\"><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<body style=\"font:16px sans-serif;padding:24px\"><h2>Đã lưu Wi-Fi</h2>"
        "<p>ChatAI đang kết nối. Vui lòng chờ thiết bị vào mạng.</p></body></html>");
}

esp_err_t StartHttpServer() {
    if (g_http_server != nullptr) return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
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

    httpd_uri_t wifi_scan_uri{};
    wifi_scan_uri.uri = "/api/wifi/scan";
    wifi_scan_uri.method = HTTP_GET;
    wifi_scan_uri.handler = WifiScanHandler;

    httpd_uri_t captive_uri{};
    captive_uri.uri = "/*";
    captive_uri.method = HTTP_GET;
    captive_uri.handler = RootHandler;

    result = httpd_register_uri_handler(g_http_server, &root_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(g_http_server, &wifi_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(g_http_server, &wifi_scan_uri);
    if (result == ESP_OK) result = chatai::web_admin::RegisterRoutes(g_http_server, g_form_token);
    if (result == ESP_OK) result = httpd_register_uri_handler(g_http_server, &captive_uri);
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
    std::snprintf(ssid, sizeof(ssid), "ChatAI-TPIOS-%02X%02X%02X", mac[3], mac[4], mac[5]);
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
    if (result == ESP_OK) result = StartDnsServer();
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
        g_internet_available.store(false);
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
        g_internet_available.store(false);
        g_connected.store(true);
        ESP_LOGI(kTag, "Wi-Fi received IPv4: " IPSTR " gateway: " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
        if (g_provisioning_started.load()) StopProvisioning();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        g_connected.store(false);
        g_internet_available.store(false);
        ESP_LOGW(kTag, "Wi-Fi station lost its IPv4 address");
    }
}
}  // namespace

namespace chatai::network {

esp_err_t NetworkManager::ClearSavedCredentials() {
    esp_err_t result = EraseCredentialNamespace(kNvsNamespace);
    if (result == ESP_OK) result = EraseCredentialNamespace("wifi");
    return result;
}

esp_err_t NetworkManager::Initialize() {
    if (g_initialized) return ESP_OK;

    std::string ssid, password;
    g_has_credentials = LoadCredentials(ssid, password);
    g_sta_netif = esp_netif_create_default_wifi_sta();
    if (g_sta_netif == nullptr) return ESP_FAIL;

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t result = esp_wifi_init(&wifi_init_config);
    if (result != ESP_OK) return result;

    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result != ESP_OK) return result;

    result = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &EventHandler, nullptr, &g_wifi_handler);
    if (result != ESP_OK) return result;
    result = esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &EventHandler, nullptr, &g_ip_handler);
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

    // Mark the manager ready before starting Wi-Fi: STA events may arrive
    // immediately from esp_wifi_start(), and must be able to start fallback AP.
    g_initialized = true;
    result = esp_wifi_start();
    if (result != ESP_OK) {
        g_initialized = false;
        return result;
    }

    char token[17]{};
    std::snprintf(token, sizeof(token), "%08lx%08lx",
                  static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()));
    g_form_token = token;

    // Start the setup AP before the HTTP server so a server startup
    // failure cannot silently leave a first-boot device without its Wi-Fi AP.
    if (!g_has_credentials) {
        result = StartProvisioning();
        if (result != ESP_OK) return result;
    }

    result = StartHttpServer();
    if (result != ESP_OK) return result;

    if (xTaskCreate(InternetCheckTask, "chatai_netcheck", 4096,
                    nullptr, 4, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
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
    g_internet_available.store(false);
    return esp_wifi_connect();
}

bool NetworkManager::IsConnected() {
    return g_connected.load();
}

bool NetworkManager::IsInternetAvailable() {
    return g_internet_available.load();
}

std::string NetworkManager::GetIpAddress() {
    if (!g_connected.load() || g_sta_netif == nullptr) return {};
    esp_netif_ip_info_t ip_info{};
    if (esp_netif_get_ip_info(g_sta_netif, &ip_info) != ESP_OK) return {};
    char address[16]{};
    std::snprintf(address, sizeof(address), IPSTR, IP2STR(&ip_info.ip));
    return address;
}

std::string NetworkManager::GetGatewayAddress() {
    if (!g_connected.load() || g_sta_netif == nullptr) return {};
    esp_netif_ip_info_t ip_info{};
    if (esp_netif_get_ip_info(g_sta_netif, &ip_info) != ESP_OK ||
        ip_info.gw.addr == 0) return {};
    char address[16]{};
    std::snprintf(address, sizeof(address), IPSTR, IP2STR(&ip_info.gw));
    return address;
}

bool NetworkManager::IsProvisioningActive() {
    return g_provisioning_started.load();
}

bool NetworkManager::HasSavedCredentials() {
    return g_has_credentials.load();
}

std::string NetworkManager::GetProvisioningSsid() {
    return g_provisioning_ssid;
}

}  // namespace chatai::network
