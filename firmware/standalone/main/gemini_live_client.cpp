#include "gemini_live_client.h"

#include <algorithm>
#include <atomic>
#include <string>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "gemini_settings.h"
#include "network_manager.h"

namespace {
constexpr char kTag[] = "ChatAI-Gemini";
constexpr char kEndpoint[] =
    "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta."
    "GenerativeService.BidiGenerateContent?key=";
constexpr size_t kMaxIncomingMessage = 8192;
constexpr uint32_t kRetryDelayMs = 5000;

std::atomic<bool> g_ready{false};
std::atomic<bool> g_task_started{false};
esp_websocket_client_handle_t g_client = nullptr;
std::string g_incoming_message;
bool g_receiving_text = false;
bool g_missing_key_reported = false;

std::string UrlEncode(const std::string& value) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (const unsigned char c : value) {
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[c >> 4]);
            encoded.push_back(kHex[c & 0x0F]);
        }
    }
    return encoded;
}

const char kSetupMessage[] =
    "{\"setup\":{"
      "\"model\":\"models/gemini-3.8-live\","
      "\"generationConfig\":{"
        "\"responseModalities\":[\"AUDIO\"]"
      "},"
      "\"systemInstruction\":{"
        "\"parts\":[{"
          "\"text\":\"Bạn là trợ lý ChatAI TPIOS. Hãy giao tiếp tự nhiên, thân thiện "
          "và rõ ràng bằng tiếng Việt. Chỉ trả lời bằng giọng nói trong phiên hội thoại "
          "thời gian thực.\""
        "}]"
      "}"
    "}}";

void HandleServerMessage(const std::string& message) {
    cJSON* root = cJSON_ParseWithLength(message.c_str(), message.size());
    if (root == nullptr) {
        ESP_LOGW(kTag, "Gemini sent an invalid JSON message");
        return;
    }

    const cJSON* setup_complete = cJSON_GetObjectItemCaseSensitive(root, "setupComplete");
    if (cJSON_IsObject(setup_complete)) {
        g_ready.store(true);
        ESP_LOGI(kTag, "Gemini Live setupComplete; session is ready");
    } else if (cJSON_GetObjectItemCaseSensitive(root, "error") != nullptr) {
        g_ready.store(false);
        ESP_LOGE(kTag, "Gemini rejected the Live session setup");
    }

    cJSON_Delete(root);
}

void WebSocketEventHandler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
    auto* event = static_cast<esp_websocket_event_data_t*>(event_data);

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED: {
            g_ready.store(false);
            g_incoming_message.clear();
            g_receiving_text = false;
            ESP_LOGI(kTag, "Gemini Live WebSocket connected; sending setup");

            const int sent = esp_websocket_client_send_text(
                g_client, kSetupMessage, sizeof(kSetupMessage) - 1,
                pdMS_TO_TICKS(5000));
            if (sent != static_cast<int>(sizeof(kSetupMessage) - 1)) {
                ESP_LOGE(kTag, "Could not send Gemini Live setup message");
            }
            break;
        }

        case WEBSOCKET_EVENT_DATA:
            if (event == nullptr || event->data_ptr == nullptr ||
                event->data_len <= 0 || event->payload_len <= 0 ||
                event->payload_len > static_cast<int>(kMaxIncomingMessage) ||
                event->payload_offset < 0) {
                break;
            }

            if (event->payload_offset == 0) {
                g_incoming_message.clear();
                g_receiving_text = event->op_code == 0x1;
            }

            if (!g_receiving_text ||
                static_cast<size_t>(event->payload_offset) != g_incoming_message.size() ||
                g_incoming_message.size() + static_cast<size_t>(event->data_len) >
                    kMaxIncomingMessage) {
                g_incoming_message.clear();
                g_receiving_text = false;
                break;
            }

            g_incoming_message.append(event->data_ptr,
                                      static_cast<size_t>(event->data_len));
            if (event->fin) {
                HandleServerMessage(g_incoming_message);
                g_incoming_message.clear();
                g_receiving_text = false;
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            g_ready.store(false);
            if (event != nullptr) {
                ESP_LOGE(kTag,
                         "WebSocket error: tls=%s tls_stack=%d cert_flags=0x%x "
                         "http_status=%d socket_errno=%d",
                         esp_err_to_name(event->error_handle.esp_tls_last_esp_err),
                         event->error_handle.esp_tls_stack_err,
                         event->error_handle.esp_tls_cert_verify_flags,
                         event->error_handle.esp_ws_handshake_status_code,
                         event->error_handle.esp_transport_sock_errno);
            } else {
                ESP_LOGE(kTag, "Gemini Live WebSocket transport error");
            }
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            g_ready.store(false);
            ESP_LOGW(kTag, "Gemini Live WebSocket disconnected");
            break;

        case WEBSOCKET_EVENT_CLOSED:
            g_ready.store(false);
            ESP_LOGW(kTag, "Gemini Live WebSocket closed; code=%d",
                     event != nullptr ? event->close_status_code : 0);
            break;

        default:
            break;
    }
}

void GeminiWorker(void*) {
    while (true) {
        if (g_client != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (!chatai::network::NetworkManager::IsConnected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        std::string api_key = chatai::settings::GetGeminiApiKey();
        if (api_key.empty()) {
            if (!g_missing_key_reported) {
                ESP_LOGW(kTag, "Gemini API Key is not configured; waiting for Web Admin");
                g_missing_key_reported = true;
            }
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
            continue;
        }

        g_missing_key_reported = false;
        std::string uri = std::string(kEndpoint) + UrlEncode(api_key);
        std::fill(api_key.begin(), api_key.end(), '\0');
        api_key.clear();

        esp_websocket_client_config_t config{};
        config.uri = uri.c_str();
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.buffer_size = 4096;
        config.task_stack = 6144;
        config.task_prio = 5;
        config.network_timeout_ms = 15000;
        config.reconnect_timeout_ms = 5000;
        config.ping_interval_sec = 20;
        config.disable_auto_reconnect = false;
        config.enable_close_reconnect = true;

        esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
        if (client == nullptr) {
            ESP_LOGE(kTag, "Could not initialize Gemini Live WebSocket client");
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
            continue;
        }

        g_client = client;
        esp_err_t result = esp_websocket_register_events(
            client, WEBSOCKET_EVENT_ANY, WebSocketEventHandler, nullptr);
        if (result == ESP_OK) {
            result = esp_websocket_client_start(client);
        }

        if (result != ESP_OK) {
            ESP_LOGE(kTag, "Could not start Gemini Live WebSocket: %s",
                     esp_err_to_name(result));
            g_client = nullptr;
            esp_websocket_client_destroy(client);
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
            continue;
        }

        ESP_LOGI(kTag, "Gemini Live connection started; waiting for setupComplete");
    }
}

}  // namespace

namespace chatai::gemini {

esp_err_t Initialize() {
    bool expected = false;
    if (!g_task_started.compare_exchange_strong(expected, true)) {
        return ESP_OK;
    }

    const BaseType_t result = xTaskCreate(
        GeminiWorker, "chatai_gemini", 6144, nullptr, 5, nullptr);
    if (result != pdPASS) {
        g_task_started.store(false);
        ESP_LOGE(kTag, "Could not create Gemini Live worker task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "Gemini Live worker initialized");
    return ESP_OK;
}

bool IsReady() {
    return g_ready.load();
}

}  // namespace chatai::gemini
