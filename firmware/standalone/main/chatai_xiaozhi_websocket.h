#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "chatai_xiaozhi_ota.h"

namespace chatai::xiaozhi {

class WebSocketTransport final {
public:
    using TextHandler = std::function<void(const std::string&)>;
    using BinaryHandler = std::function<void(const std::vector<uint8_t>&)>;

    WebSocketTransport() = default;
    ~WebSocketTransport();

    WebSocketTransport(const WebSocketTransport&) = delete;
    WebSocketTransport& operator=(const WebSocketTransport&) = delete;

    esp_err_t Start(const ServerConfig& config, TextHandler text_handler,
                    BinaryHandler binary_handler);
    esp_err_t Stop();
    bool IsConnected() const { return connected_.load(); }
    esp_err_t SendText(const std::string& text);
    esp_err_t SendBinary(const uint8_t* data, size_t size);

private:
    enum class MessageType : uint8_t { kNone, kText, kBinary };

    static void EventHandler(void* context, esp_event_base_t base,
                             int32_t event_id, void* event_data);
    void HandleEvent(int32_t event_id, esp_websocket_event_data_t* event);
    void HandleData(esp_websocket_event_data_t* event);

    static constexpr EventBits_t kConnected = 1 << 0;
    static constexpr EventBits_t kFailed = 1 << 1;
    static constexpr size_t kMaxMessageBytes = 16384;

    esp_websocket_client_handle_t client_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    std::string url_;
    std::string headers_;
    std::atomic<bool> connected_{false};
    TextHandler text_handler_;
    BinaryHandler binary_handler_;

    std::mutex send_mutex_;
    std::mutex message_mutex_;
    std::string incoming_message_;
    size_t incoming_frame_bytes_ = 0;
    MessageType incoming_type_ = MessageType::kNone;
};

}  // namespace chatai::xiaozhi
