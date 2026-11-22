#include "chatai_xiaozhi_mqtt.h"

#include <utility>

#include <esp_crt_bundle.h>
#include <esp_log.h>

namespace chatai::xiaozhi {
namespace {

constexpr char kTag[] = "XiaozhiMQTT";
constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr int kDefaultPort = 8883;

bool BuildBrokerUri(const std::string& endpoint, std::string* uri) {
    if (uri == nullptr || endpoint.empty()) return false;

    if (endpoint.rfind("mqtts://", 0) == 0) {
        *uri = endpoint;
        return true;
    }
    if (endpoint.find("://") != std::string::npos) {
        return false;
    }

    *uri = "mqtts://" + endpoint;
    if (endpoint.find(':') == std::string::npos) {
        *uri += ":" + std::to_string(kDefaultPort);
    }
    return true;
}

}  // namespace

MqttTransport::~MqttTransport() {
    Stop();
    if (events_ != nullptr) {
        vEventGroupDelete(events_);
        events_ = nullptr;
    }
}

esp_err_t MqttTransport::Start(const ServerConfig& config,
                               MessageHandler handler) {
    if (config.transport != Transport::kMqttUdp ||
        config.mqtt_endpoint.empty() || config.mqtt_client_id.empty() ||
        config.mqtt_username.empty() || config.mqtt_password.empty() ||
        config.mqtt_publish_topic.empty() || config.mqtt_subscribe_topic.empty() ||
        !handler) {
        return ESP_ERR_INVALID_ARG;
    }

    if (client_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    if (events_ == nullptr) {
        events_ = xEventGroupCreate();
        if (events_ == nullptr) return ESP_ERR_NO_MEM;
    }
    xEventGroupClearBits(events_, kConnected | kStartFailed);

    std::string broker_uri;
    if (!BuildBrokerUri(config.mqtt_endpoint, &broker_uri)) {
        ESP_LOGE(kTag, "MQTT endpoint must use TLS");
        return ESP_ERR_INVALID_ARG;
    }

    publish_topic_ = config.mqtt_publish_topic;
    subscribe_topic_ = config.mqtt_subscribe_topic;
    message_handler_ = std::move(handler);
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        incoming_topic_.clear();
        incoming_payload_.clear();
    }

    esp_mqtt_client_config_t mqtt_config{};
    mqtt_config.broker.address.uri = broker_uri.c_str();
    mqtt_config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    mqtt_config.credentials.client_id = config.mqtt_client_id.c_str();
    mqtt_config.credentials.username = config.mqtt_username.c_str();
    mqtt_config.credentials.authentication.password = config.mqtt_password.c_str();
    mqtt_config.session.keepalive = config.mqtt_keepalive > 0
        ? config.mqtt_keepalive : 240;
    mqtt_config.network.timeout_ms = 10000;
    mqtt_config.network.reconnect_timeout_ms = 5000;

    client_ = esp_mqtt_client_init(&mqtt_config);
    if (client_ == nullptr) {
        message_handler_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_mqtt_client_register_event(
        client_, MQTT_EVENT_ANY, EventHandler, this);
    if (result != ESP_OK) {
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
        message_handler_ = nullptr;
        return result;
    }

    result = esp_mqtt_client_start(client_);
    if (result != ESP_OK) {
        esp_mqtt_client_destroy(client_);
        client_ = nullptr;
        message_handler_ = nullptr;
        return result;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        events_, kConnected | kStartFailed, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(kConnectTimeoutMs));
    if ((bits & kConnected) == 0) {
        Stop();
        return (bits & kStartFailed) != 0 ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(kTag, "MQTT TLS connected and response topic subscribed");
    return ESP_OK;
}

esp_err_t MqttTransport::Stop() {
    connected_.store(false);
    if (client_ == nullptr) {
        message_handler_ = nullptr;
        return ESP_OK;
    }

    const esp_err_t stop_result = esp_mqtt_client_stop(client_);
    const esp_err_t destroy_result = esp_mqtt_client_destroy(client_);
    client_ = nullptr;
    message_handler_ = nullptr;
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        incoming_topic_.clear();
        incoming_payload_.clear();
    }

    if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
        return stop_result;
    }
    return destroy_result;
}

esp_err_t MqttTransport::Publish(const std::string& payload) {
    if (client_ == nullptr || !connected_.load()) {
        return ESP_ERR_INVALID_STATE;
    }
    const int message_id = esp_mqtt_client_publish(
        client_, publish_topic_.c_str(), payload.c_str(),
        static_cast<int>(payload.size()), 0, 0);
    return message_id >= 0 ? ESP_OK : ESP_FAIL;
}

void MqttTransport::EventHandler(void* context, esp_event_base_t,
                                 int32_t, void* event_data) {
    auto* transport = static_cast<MqttTransport*>(context);
    auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
    if (transport != nullptr && event != nullptr) {
        transport->HandleEvent(event);
    }
}

void MqttTransport::HandleEvent(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED: {
            const int message_id = esp_mqtt_client_subscribe(
                client_, subscribe_topic_.c_str(), 0);
            if (message_id < 0) {
                connected_.store(false);
                xEventGroupSetBits(events_, kStartFailed);
                ESP_LOGE(kTag, "Could not subscribe to the Xiaozhi response topic");
                return;
            }
            connected_.store(true);
            xEventGroupSetBits(events_, kConnected);
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            connected_.store(false);
            xEventGroupClearBits(events_, kConnected);
            ESP_LOGW(kTag, "MQTT disconnected");
            break;

        case MQTT_EVENT_DATA: {
            MessageHandler handler;
            std::string topic;
            std::string payload;
            bool complete = false;

            {
                std::lock_guard<std::mutex> lock(message_mutex_);
                if (event->current_data_offset == 0) {
                    incoming_topic_.clear();
                    incoming_payload_.clear();
                    if (event->topic != nullptr && event->topic_len > 0) {
                        incoming_topic_.assign(event->topic,
                                               static_cast<size_t>(event->topic_len));
                    }
                }

                if (event->current_data_offset !=
                    static_cast<int>(incoming_payload_.size())) {
                    incoming_topic_.clear();
                    incoming_payload_.clear();
                    return;
                }

                if (event->data_len < 0 ||
                    static_cast<size_t>(event->data_len) >
                        kMaxMessageBytes - incoming_payload_.size() ||
                    event->total_data_len < 0 ||
                    static_cast<size_t>(event->total_data_len) > kMaxMessageBytes) {
                    incoming_topic_.clear();
                    incoming_payload_.clear();
                    ESP_LOGW(kTag, "Dropped oversized MQTT message");
                    return;
                }

                if (event->data != nullptr && event->data_len > 0) {
                    incoming_payload_.append(
                        event->data, static_cast<size_t>(event->data_len));
                }

                complete = incoming_payload_.size() ==
                    static_cast<size_t>(event->total_data_len);
                if (complete) {
                    topic = std::move(incoming_topic_);
                    payload = std::move(incoming_payload_);
                    handler = message_handler_;
                }
            }

            if (complete && handler) {
                handler(topic, payload);
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            ESP_LOGE(kTag, "MQTT transport reported an error");
            break;

        default:
            break;
    }
}

}  // namespace chatai::xiaozhi
