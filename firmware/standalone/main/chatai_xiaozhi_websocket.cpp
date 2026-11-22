#include "chatai_xiaozhi_websocket.h"

#include <utility>
#include <climits>

#include <esp_crt_bundle.h>
#include <esp_log.h>

namespace chatai::xiaozhi {
namespace {

constexpr char kTag[] = "XiaozhiWS";
constexpr uint32_t kConnectTimeoutMs = 15000;

}  // namespace

WebSocketTransport::~WebSocketTransport() {
    Stop();
    if (events_ != nullptr) {
        vEventGroupDelete(events_);
        events_ = nullptr;
    }
}

esp_err_t WebSocketTransport::Start(const ServerConfig& config,
                                    TextHandler text_handler,
                                    BinaryHandler binary_handler) {
    const bool secure_url = config.websocket_url.rfind("wss://", 0) == 0;
    const bool plain_url = config.websocket_url.rfind("ws://", 0) == 0;
    if (client_ != nullptr || (!secure_url && !plain_url) ||
        config.websocket_url.size() > 2048 ||
        config.device_mac_address.empty() || config.device_client_id.empty() ||
        config.websocket_token.find('\r') != std::string::npos ||
        config.websocket_token.find('\n') != std::string::npos ||
        config.websocket_token.size() > 2048 ||
        !text_handler || !binary_handler) {
        return ESP_ERR_INVALID_ARG;
    }

    if (events_ == nullptr) {
        events_ = xEventGroupCreate();
        if (events_ == nullptr) return ESP_ERR_NO_MEM;
    }
    xEventGroupClearBits(events_, kConnected | kFailed);
    connected_.store(false);
    text_handler_ = std::move(text_handler);
    binary_handler_ = std::move(binary_handler);

    std::string token = config.websocket_token;
    if (!token.empty() && token.find(' ') == std::string::npos) {
        token = "Bearer " + token;
    }
    std::string headers;
    if (!token.empty()) headers += "Authorization: " + token + "\r\n";
    headers += "Protocol-Version: " +
        std::to_string(config.websocket_version) + "\r\n";
    headers += "Device-Id: " + config.device_mac_address + "\r\n";
    headers += "Client-Id: " + config.device_client_id + "\r\n";

    esp_websocket_client_config_t ws_config{};
    ws_config.uri = config.websocket_url.c_str();
    ws_config.headers = headers.c_str();
    if (config.websocket_url.rfind("wss://", 0) == 0) {
        ws_config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    ws_config.buffer_size = 4096;
    ws_config.task_stack = 4096;
    ws_config.task_prio = 5;
    ws_config.network_timeout_ms = kConnectTimeoutMs;
    ws_config.reconnect_timeout_ms = 5000;
    ws_config.ping_interval_sec = 20;
    ws_config.disable_auto_reconnect = true;
    ws_config.enable_close_reconnect = false;

    client_ = esp_websocket_client_init(&ws_config);
    if (client_ == nullptr) {
        text_handler_ = nullptr;
        binary_handler_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_websocket_register_events(
        client_, WEBSOCKET_EVENT_ANY, EventHandler, this);
    if (result == ESP_OK) result = esp_websocket_client_start(client_);
    if (result != ESP_OK) {
        Stop();
        return result;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        events_, kConnected | kFailed, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(kConnectTimeoutMs));
    if ((bits & kConnected) == 0) {
        Stop();
        return (bits & kFailed) ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(kTag, "WebSocket connected (protocol v%d)", config.websocket_version);
    return ESP_OK;
}

esp_err_t WebSocketTransport::Stop() {
    connected_.store(false);
    esp_err_t result = ESP_OK;
    if (client_ != nullptr) {
        result = esp_websocket_client_stop(client_);
        const esp_err_t destroy_result = esp_websocket_client_destroy(client_);
        client_ = nullptr;
        if (result == ESP_OK || result == ESP_ERR_INVALID_STATE) {
            result = destroy_result;
        }
    }
    text_handler_ = nullptr;
    binary_handler_ = nullptr;
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        incoming_message_.clear();
        incoming_frame_bytes_ = 0;
        incoming_type_ = MessageType::kNone;
    }
    return result;
}

esp_err_t WebSocketTransport::SendText(const std::string& text) {
    if (text.size() > static_cast<size_t>(INT_MAX)) return ESP_ERR_INVALID_SIZE;
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (client_ == nullptr || !connected_.load()) return ESP_ERR_INVALID_STATE;
    const int sent = esp_websocket_client_send_text(
        client_, text.c_str(), static_cast<int>(text.size()), pdMS_TO_TICKS(5000));
    return sent == static_cast<int>(text.size()) ? ESP_OK : ESP_FAIL;
}

esp_err_t WebSocketTransport::SendBinary(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0 || size > static_cast<size_t>(INT_MAX))
        return ESP_ERR_INVALID_ARG;
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (client_ == nullptr || !connected_.load()) return ESP_ERR_INVALID_STATE;
    const int sent = esp_websocket_client_send_bin(
        client_, reinterpret_cast<const char*>(data), static_cast<int>(size),
        pdMS_TO_TICKS(5000));
    return sent == static_cast<int>(size) ? ESP_OK : ESP_FAIL;
}

void WebSocketTransport::EventHandler(void* context, esp_event_base_t,
                                      int32_t event_id, void* event_data) {
    auto* transport = static_cast<WebSocketTransport*>(context);
    if (transport == nullptr) return;
    transport->HandleEvent(
        event_id, static_cast<esp_websocket_event_data_t*>(event_data));
}

void WebSocketTransport::HandleEvent(int32_t event_id,
                                     esp_websocket_event_data_t* event) {
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            connected_.store(true);
            xEventGroupSetBits(events_, kConnected);
            break;

        case WEBSOCKET_EVENT_DATA:
            HandleData(event);
            break;

        case WEBSOCKET_EVENT_ERROR:
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            connected_.store(false);
            xEventGroupClearBits(events_, kConnected);
            xEventGroupSetBits(events_, kFailed);
            ESP_LOGW(kTag, "WebSocket transport disconnected (event=%ld)",
                     static_cast<long>(event_id));
            break;

        default:
            break;
    }
}

void WebSocketTransport::HandleData(esp_websocket_event_data_t* event) {
    if (event == nullptr) return;
    const uint8_t opcode = static_cast<uint8_t>(event->op_code);
    if (opcode == 0x8 || opcode == 0x9 || opcode == 0xA) return;

    std::string complete_message;
    MessageType complete_type = MessageType::kNone;
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        if (event->data_len <= 0 || event->payload_len <= 0 ||
            event->payload_offset < 0 ||
            event->payload_len > static_cast<int>(kMaxMessageBytes) ||
            event->payload_offset > event->payload_len ||
            event->data_len > event->payload_len - event->payload_offset ||
            event->data_ptr == nullptr) {
            incoming_message_.clear();
            incoming_frame_bytes_ = 0;
            incoming_type_ = MessageType::kNone;
            return;
        }

        if (event->payload_offset == 0) {
            if (opcode == 0x1 || opcode == 0x2) {
                incoming_message_.clear();
                incoming_type_ = opcode == 0x1
                    ? MessageType::kText : MessageType::kBinary;
                incoming_frame_bytes_ = 0;
            } else if (opcode == 0x0) {
                if (incoming_type_ == MessageType::kNone) return;
                incoming_frame_bytes_ = 0;
            } else {
                incoming_message_.clear();
                incoming_frame_bytes_ = 0;
                incoming_type_ = MessageType::kNone;
                return;
            }
        }

        if (incoming_type_ == MessageType::kNone ||
            static_cast<size_t>(event->payload_offset) != incoming_frame_bytes_ ||
            incoming_message_.size() + static_cast<size_t>(event->data_len) >
                kMaxMessageBytes) {
            incoming_message_.clear();
            incoming_frame_bytes_ = 0;
            incoming_type_ = MessageType::kNone;
            return;
        }

        incoming_message_.append(event->data_ptr,
                                 static_cast<size_t>(event->data_len));
        incoming_frame_bytes_ += static_cast<size_t>(event->data_len);
        if (incoming_frame_bytes_ == static_cast<size_t>(event->payload_len)) {
            if (event->fin) {
                complete_message = std::move(incoming_message_);
                complete_type = incoming_type_;
                incoming_message_.clear();
                incoming_frame_bytes_ = 0;
                incoming_type_ = MessageType::kNone;
            } else {
                incoming_frame_bytes_ = 0;
            }
        }
    }

    if (complete_type == MessageType::kText && text_handler_) {
        text_handler_(complete_message);
    } else if (complete_type == MessageType::kBinary && binary_handler_) {
        std::vector<uint8_t> bytes(complete_message.begin(), complete_message.end());
        binary_handler_(bytes);
    }
}

}  // namespace chatai::xiaozhi
