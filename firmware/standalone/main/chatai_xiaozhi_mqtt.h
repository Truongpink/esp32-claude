#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include <esp_err.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <mqtt_client.h>

#include "chatai_xiaozhi_ota.h"

namespace chatai::xiaozhi {

class MqttTransport final {
public:
    using MessageHandler =
        std::function<void(const std::string& topic, const std::string& payload)>;

    MqttTransport() = default;
    ~MqttTransport();

    MqttTransport(const MqttTransport&) = delete;
    MqttTransport& operator=(const MqttTransport&) = delete;

    esp_err_t Start(const ServerConfig& config, MessageHandler handler);
    esp_err_t Stop();
    esp_err_t Publish(const std::string& payload);
    bool IsConnected() const { return connected_.load(); }

private:
    static void EventHandler(void* context, esp_event_base_t base,
                             int32_t event_id, void* event_data);
    void HandleEvent(esp_mqtt_event_handle_t event);

    static constexpr EventBits_t kSubscribed = 1 << 0;
    static constexpr EventBits_t kStartFailed = 1 << 1;
    static constexpr size_t kMaxMessageBytes = 16384;

    esp_mqtt_client_handle_t client_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    std::atomic<bool> connected_{false};
    std::string publish_topic_;
    std::string subscribe_topic_;
    std::atomic<int> pending_subscribe_id_{-1};
    std::mutex client_mutex_;
    MessageHandler message_handler_;

    std::mutex message_mutex_;
    std::string incoming_topic_;
    std::string incoming_payload_;
};

}  // namespace chatai::xiaozhi
