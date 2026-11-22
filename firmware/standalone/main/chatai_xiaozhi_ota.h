#pragma once

#include <cstdint>
#include <string>

#include <esp_err.h>

namespace chatai::xiaozhi {

enum class Transport : uint8_t {
    kNone,
    kMqttUdp,
    kWebSocket,
};

struct ServerConfig {
    Transport transport = Transport::kNone;
    std::string device_mac_address;
    std::string device_client_id;

    // MQTT control channel. UDP audio parameters/keys arrive in server hello.
    std::string mqtt_endpoint;
    std::string mqtt_client_id;
    std::string mqtt_username;
    std::string mqtt_password;
    std::string mqtt_publish_topic;
    std::string mqtt_subscribe_topic;
    int mqtt_keepalive = 240;

    // WebSocket channel.
    std::string websocket_url;
    std::string websocket_token;
    int websocket_version = 1;

    bool activation_required = false;
    std::string activation_message;
    std::string activation_code;
    std::string activation_challenge;
};

// Queries the public Xiaozhi OTA configuration endpoint. The request uses a
// stable UUID v4 stored in ChatAI's own NVS namespace and never logs credentials.
esp_err_t FetchServerConfig(ServerConfig* config);
esp_err_t CheckActivation();

}  // namespace chatai::xiaozhi
