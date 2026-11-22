#include "chatai_xiaozhi_ota.h"

#include "network_manager.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/time.h>
#include <string>
#include <utility>

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_flash.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_netif_sntp.h>
#include <freertos/FreeRTOS.h>
#include <nvs.h>

namespace chatai::xiaozhi {
namespace {

constexpr char kTag[] = "XiaozhiOTA";
constexpr char kOtaUrl[] = "https://api.tenclass.net/xiaozhi/ota/";
constexpr char kNvsNamespace[] = "chatai_xiaozhi";
constexpr char kClientIdKey[] = "client_id";
constexpr char kLanguage[] = "vi-VN";
constexpr size_t kMaxResponseBytes = 16384;

struct HttpResponse {
    std::string body;
    bool overflow = false;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    if (event == nullptr || event->user_data == nullptr) return ESP_OK;
    auto* response = static_cast<HttpResponse*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data != nullptr &&
        event->data_len > 0) {
        const size_t count = static_cast<size_t>(event->data_len);
        if (count > kMaxResponseBytes - response->body.size()) {
            response->overflow = true;
            return ESP_FAIL;
        }
        response->body.append(static_cast<const char*>(event->data), count);
    }
    return ESP_OK;
}

std::string GetClientId() {
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) return {};

    size_t size = 0;
    if (nvs_get_str(handle, kClientIdKey, nullptr, &size) == ESP_OK &&
        size == 37) {
        std::string id(size, '\0');
        const esp_err_t read_result = nvs_get_str(handle, kClientIdKey, id.data(), &size);
        nvs_close(handle);
        if (read_result == ESP_OK) {
            id.resize(36);
            return id;
        }
        return {};
    }

    uint8_t bytes[16]{};
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);

    char id[37]{};
    std::snprintf(id, sizeof(id),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
        bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12],
        bytes[13], bytes[14], bytes[15]);

    const esp_err_t save_result = nvs_set_str(handle, kClientIdKey, id);
    const esp_err_t commit_result =
        save_result == ESP_OK ? nvs_commit(handle) : save_result;
    nvs_close(handle);
    return commit_result == ESP_OK ? std::string(id) : std::string();
}

std::string GetMacAddress() {
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return {};
    char value[18]{};
    std::snprintf(value, sizeof(value), "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return value;
}

esp_err_t EnsureSystemTime() {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    const std::time_t now = std::time(nullptr);
    if (now >= 1735689600 && now < 4102444800) return ESP_OK;

    const esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("time.google.com", "pool.ntp.org",
                               "time.cloudflare.com"));
    const esp_err_t init_result = esp_netif_sntp_init(&config);
    if (init_result != ESP_OK && init_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "Could not initialize SNTP: %s", esp_err_to_name(init_result));
        return init_result;
    }
    const esp_err_t sync_result =
        esp_netif_sntp_sync_wait(pdMS_TO_TICKS(12000));
    if (init_result == ESP_OK) esp_netif_sntp_deinit();
    if (sync_result != ESP_OK) {
        ESP_LOGW(kTag, "System time is not synchronized: %s",
                 esp_err_to_name(sync_result));
        return sync_result;
    }
    ESP_LOGI(kTag, "System time synchronized for TLS certificate validation");
    return ESP_OK;
}

void AddString(cJSON* object, const char* key, const std::string& value) {
    if (!value.empty()) cJSON_AddStringToObject(object, key, value.c_str());
}

bool AddDeviceInfo(cJSON* root, const std::string& mac,
                   const std::string& client_id) {
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "language", kLanguage);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(nullptr, &flash_size) == ESP_OK) {
        cJSON_AddNumberToObject(root, "flash_size", flash_size);
    }

    char heap[24]{};
    std::snprintf(heap, sizeof(heap), "%lu",
                  static_cast<unsigned long>(esp_get_minimum_free_heap_size()));
    cJSON_AddStringToObject(root, "minimum_free_heap_size", heap);
    cJSON_AddStringToObject(root, "mac_address", mac.c_str());
    cJSON_AddStringToObject(root, "uuid", client_id.c_str());
    cJSON_AddStringToObject(root, "chip_model_name", CONFIG_IDF_TARGET);

    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    cJSON* chip_info = cJSON_AddObjectToObject(root, "chip_info");
    if (chip_info == nullptr) return false;
    cJSON_AddNumberToObject(chip_info, "model", static_cast<int>(chip.model));
    cJSON_AddNumberToObject(chip_info, "cores", chip.cores);
    cJSON_AddNumberToObject(chip_info, "revision", chip.revision);
    cJSON_AddNumberToObject(chip_info, "features", chip.features);

    const esp_app_desc_t* app = esp_app_get_description();
    cJSON* application = cJSON_AddObjectToObject(root, "application");
    if (app == nullptr || application == nullptr) return false;
    cJSON_AddStringToObject(application, "name", "ChatAI TPIOS");
    cJSON_AddStringToObject(application, "version", app->version);
    char compile_time[48]{};
    std::snprintf(compile_time, sizeof(compile_time), "%sT%sZ", app->date, app->time);
    cJSON_AddStringToObject(application, "compile_time", compile_time);
    cJSON_AddStringToObject(application, "idf_version", app->idf_ver);
    char elf_sha256[65]{};
    for (size_t i = 0; i < sizeof(app->app_elf_sha256); ++i) {
        std::snprintf(elf_sha256 + i * 2, sizeof(elf_sha256) - i * 2,
                      "%02x", app->app_elf_sha256[i]);
    }
    cJSON_AddStringToObject(application, "elf_sha256", elf_sha256);

    cJSON* partitions = cJSON_AddArrayToObject(root, "partition_table");
    if (partitions == nullptr) return false;
    esp_partition_iterator_t iterator = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (iterator != nullptr) {
        const esp_partition_t* partition = esp_partition_get(iterator);
        if (partition != nullptr) {
            cJSON* item = cJSON_CreateObject();
            if (item == nullptr) {
                esp_partition_iterator_release(iterator);
                return false;
            }
            cJSON_AddStringToObject(item, "label", partition->label);
            cJSON_AddNumberToObject(item, "type", partition->type);
            cJSON_AddNumberToObject(item, "subtype", partition->subtype);
            cJSON_AddNumberToObject(item, "address", partition->address);
            cJSON_AddNumberToObject(item, "size", partition->size);
            cJSON_AddItemToArray(partitions, item);
        }
        iterator = esp_partition_next(iterator);
    }

    cJSON* ota = cJSON_AddObjectToObject(root, "ota");
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (ota == nullptr || running == nullptr) return false;
    cJSON_AddStringToObject(ota, "label", running->label);

    cJSON* board = cJSON_AddObjectToObject(root, "board");
    if (board == nullptr) return false;
    cJSON_AddStringToObject(board, "type", "chatai-tpios-esp32s3");
    cJSON_AddStringToObject(board, "name", "ChatAI TPIOS");
    cJSON_AddStringToObject(board, "manufacturer", "TPIOS");
    AddString(board, "ssid", chatai::network::NetworkManager::GetSsid());
    AddString(board, "ip", chatai::network::NetworkManager::GetIpAddress());
    AddString(board, "mac", mac);
    cJSON_AddNumberToObject(board, "rssi",
                            chatai::network::NetworkManager::GetRssi());

    cJSON* protocol = cJSON_AddObjectToObject(root, "protocol");
    cJSON* features = protocol == nullptr ? nullptr :
        cJSON_AddObjectToObject(protocol, "features");
    if (features == nullptr) return false;
    // ChatAI has not integrated MCP, AEC, UDP probing or glyph push yet.
    cJSON_AddBoolToObject(features, "mcp", false);
    cJSON_AddBoolToObject(features, "mcp_20260728", false);
    cJSON_AddBoolToObject(features, "udp_probe", false);
    cJSON_AddBoolToObject(features, "aec", false);
    cJSON_AddBoolToObject(features, "glyph_push", false);
    return true;
}

bool ReadString(const cJSON* object, const char* key, std::string* value) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr ||
        item->valuestring[0] == '\0') return false;
    *value = item->valuestring;
    return true;
}

void ReadOptionalString(const cJSON* object, const char* key,
                        std::string* value) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        *value = item->valuestring;
    }
}

bool ParseServerConfig(const cJSON* root, ServerConfig* config) {
    const cJSON* activation = cJSON_GetObjectItemCaseSensitive(root, "activation");
    if (cJSON_IsObject(activation)) {
        config->activation_required = true;
        const cJSON* message = cJSON_GetObjectItemCaseSensitive(activation, "message");
        const cJSON* code = cJSON_GetObjectItemCaseSensitive(activation, "code");
        if (cJSON_IsString(message)) config->activation_message = message->valuestring;
        if (cJSON_IsString(code)) config->activation_code = code->valuestring;
        const cJSON* challenge =
            cJSON_GetObjectItemCaseSensitive(activation, "challenge");
        if (cJSON_IsString(challenge))
            config->activation_challenge = challenge->valuestring;
    }

    const cJSON* mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        if (!ReadString(mqtt, "endpoint", &config->mqtt_endpoint) ||
            !ReadString(mqtt, "client_id", &config->mqtt_client_id) ||
            !ReadString(mqtt, "username", &config->mqtt_username) ||
            !ReadString(mqtt, "publish_topic", &config->mqtt_publish_topic)) {
            return false;
        }
        ReadOptionalString(mqtt, "password", &config->mqtt_password);
        // Newer OTA servers provide the complete P2P topic
        // (for example devices/p2p/<mac>). Preserve it exactly. Older
        // responses may provide only the root (for example "devices");
        // in that case derive the gateway topic from the MAC address.
        std::string subscribe_topic;
        if (ReadString(mqtt, "subscribe_topic", &subscribe_topic)) {
            const size_t p2p = subscribe_topic.find("/p2p/");
            if (p2p != std::string::npos && p2p + 5 < subscribe_topic.size()) {
                config->mqtt_subscribe_topic = subscribe_topic;
            } else {
                const size_t separator = subscribe_topic.find('/');
                if (separator != std::string::npos)
                    subscribe_topic.resize(separator);
                if (!subscribe_topic.empty() &&
                    !config->device_mac_address.empty()) {
                    std::string safe_mac = config->device_mac_address;
                    for (char& character : safe_mac) {
                        if (character == ':') character = '_';
                    }
                    config->mqtt_subscribe_topic =
                        subscribe_topic + "/p2p/" + safe_mac;
                }
            }
        }
        if (config->mqtt_subscribe_topic.empty() &&
            !config->device_mac_address.empty()) {
            std::string safe_mac = config->device_mac_address;
            for (char& character : safe_mac) {
                if (character == ':') character = '_';
            }
            // Current Xiaozhi gateway P2P routing convention.
            config->mqtt_subscribe_topic = "devices/p2p/" + safe_mac;
        }

        const cJSON* keepalive =
            cJSON_GetObjectItemCaseSensitive(mqtt, "keepalive");
        if (cJSON_IsNumber(keepalive) && keepalive->valueint > 0 &&
            keepalive->valueint <= 3600) {
            config->mqtt_keepalive = keepalive->valueint;
        }
        config->transport = Transport::kMqttUdp;
        return true;
    }

    const cJSON* websocket = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        if (!ReadString(websocket, "url", &config->websocket_url)) {
            return false;
        }
        ReadOptionalString(websocket, "token", &config->websocket_token);
        const cJSON* version =
            cJSON_GetObjectItemCaseSensitive(websocket, "version");
        if (version != nullptr) {
            if (!cJSON_IsNumber(version) || version->valueint < 1 ||
                version->valueint > 3) return false;
            config->websocket_version = version->valueint;
        }
        config->transport = Transport::kWebSocket;
        return true;
    }

    return config->activation_required;
}

}  // namespace

esp_err_t CheckActivation(const ServerConfig& config) {
    if (!config.activation_required || config.activation_challenge.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!chatai::network::NetworkManager::IsConnected()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t time_result = EnsureSystemTime();
    if (time_result != ESP_OK) return time_result;

    const std::string mac = GetMacAddress();
    const std::string client_id = GetClientId();
    if (mac.empty() || client_id.empty()) return ESP_FAIL;

    HttpResponse response;
    const std::string activation_url = std::string(kOtaUrl) + "activate";
    esp_http_client_config_t http_config{};
    http_config.url = activation_url.c_str();
    http_config.method = HTTP_METHOD_POST;
    http_config.timeout_ms = 12000;
    http_config.buffer_size = 1024;
    http_config.buffer_size_tx = 256;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.event_handler = HttpEventHandler;
    http_config.user_data = &response;

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == nullptr) return ESP_ERR_NO_MEM;

    const std::string user_agent =
        std::string("ChatAI-TPIOS/") + esp_app_get_description()->version;
    esp_http_client_set_header(client, "Device-Id", mac.c_str());
    esp_http_client_set_header(client, "Client-Id", client_id.c_str());
    esp_http_client_set_header(client, "User-Agent", user_agent.c_str());
    esp_http_client_set_header(client, "Accept-Language", kLanguage);
    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, "{}", 2);

    const esp_err_t request_result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (response.overflow) return ESP_ERR_INVALID_SIZE;
    if (request_result != ESP_OK) return request_result;
    if (status == 200) {
        ESP_LOGI(kTag, "Xiaozhi activation completed");
        return ESP_OK;
    }
    if (status == 202) return ESP_ERR_NOT_FINISHED;

    ESP_LOGW(kTag, "Xiaozhi activation HTTP status: %d", status);
    return ESP_FAIL;
}

esp_err_t FetchServerConfig(ServerConfig* config) {
    if (config == nullptr) return ESP_ERR_INVALID_ARG;
    *config = ServerConfig{};

    if (!chatai::network::NetworkManager::IsConnected()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t time_result = EnsureSystemTime();
    if (time_result != ESP_OK) return time_result;

    const std::string mac = GetMacAddress();
    const std::string client_id = GetClientId();
    if (mac.empty() || client_id.empty()) {
        ESP_LOGE(kTag, "Could not establish persistent device identity");
        return ESP_FAIL;
    }
    config->device_mac_address = mac;
    config->device_client_id = client_id;

    cJSON* request = cJSON_CreateObject();
    if (request == nullptr) return ESP_ERR_NO_MEM;
    const bool request_ready = AddDeviceInfo(request, mac, client_id);
    char* request_body = request_ready ? cJSON_PrintUnformatted(request) : nullptr;
    cJSON_Delete(request);
    if (request_body == nullptr) return ESP_ERR_NO_MEM;

    HttpResponse response;
    esp_http_client_config_t http_config{};
    http_config.url = kOtaUrl;
    http_config.method = HTTP_METHOD_POST;
    http_config.timeout_ms = 12000;
    http_config.buffer_size = 2048;
    http_config.buffer_size_tx = 2048;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.event_handler = HttpEventHandler;
    http_config.user_data = &response;

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == nullptr) {
        cJSON_free(request_body);
        return ESP_ERR_NO_MEM;
    }

    const std::string user_agent =
        std::string("ChatAI-TPIOS/") + esp_app_get_description()->version;
    esp_http_client_set_header(client, "Device-Id", mac.c_str());
    esp_http_client_set_header(client, "Client-Id", client_id.c_str());
    esp_http_client_set_header(client, "User-Agent", user_agent.c_str());
    esp_http_client_set_header(client, "Accept-Language", kLanguage);
    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, request_body,
                                   static_cast<int>(std::strlen(request_body)));

    esp_err_t result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    cJSON_free(request_body);

    if (response.overflow) return ESP_ERR_INVALID_SIZE;
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "OTA configuration request failed: %s",
                 esp_err_to_name(result));
        return result;
    }
    if (status != 200) {
        ESP_LOGE(kTag, "OTA configuration HTTP status: %d", status);
        return ESP_FAIL;
    }

    cJSON* root = cJSON_ParseWithLength(response.body.data(), response.body.size());
    if (root == nullptr) {
        ESP_LOGE(kTag, "OTA configuration response is not valid JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON* server_time = cJSON_GetObjectItemCaseSensitive(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        const cJSON* timestamp = cJSON_GetObjectItemCaseSensitive(server_time, "timestamp");
        const cJSON* timezone_offset =
            cJSON_GetObjectItemCaseSensitive(server_time, "timezone_offset");
        if (cJSON_IsNumber(timestamp) && timestamp->valuedouble > 1735689600000.0 &&
            timestamp->valuedouble < 4102444800000.0 &&
            (timezone_offset == nullptr ||
             (cJSON_IsNumber(timezone_offset) && timezone_offset->valueint >= -840 &&
              timezone_offset->valueint <= 840))) {
            const int64_t timestamp_ms = static_cast<int64_t>(timestamp->valuedouble);
            const int64_t offset_ms = cJSON_IsNumber(timezone_offset)
                ? static_cast<int64_t>(timezone_offset->valueint) * 60000 : 0;
            const int64_t local_ms = timestamp_ms + offset_ms;
            timeval system_time{};
            system_time.tv_sec = static_cast<time_t>(local_ms / 1000);
            system_time.tv_usec = static_cast<suseconds_t>((local_ms % 1000) * 1000);
            if (settimeofday(&system_time, nullptr) != 0) {
                ESP_LOGW(kTag, "Could not apply Xiaozhi server time");
            } else {
                ESP_LOGI(kTag, "System time synchronized from Xiaozhi OTA response");
            }
        } else {
            ESP_LOGW(kTag, "Ignoring invalid Xiaozhi server_time fields");
        }
    }

    const bool parsed = ParseServerConfig(root, config);
    cJSON_Delete(root);
    if (!parsed) {
        ESP_LOGE(kTag, "OTA response has no supported MQTT or WebSocket configuration");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (config->activation_required) {
        ESP_LOGW(kTag, "Xiaozhi device activation is required");
        return ESP_ERR_NOT_FINISHED;
    }

    ESP_LOGI(kTag, "Xiaozhi OTA configuration received; selected transport=%s",
             config->transport == Transport::kMqttUdp ? "mqtt+udp" : "websocket");
    return ESP_OK;
}

}  // namespace chatai::xiaozhi
