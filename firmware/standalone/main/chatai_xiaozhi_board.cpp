#include "board.h"
#include "display.h"

#include "chatai_xiaozhi_audio_codec.h"
#include "network_manager.h"
#include "esp_network.h"
#include "system_info.h"
#include "display_manager.h"

#include <esp_log.h>
#include <cJSON.h>

#include <mutex>
#include <string>
#include <utility>

namespace {
constexpr char kTag[] = "ChatAI-Board";

class ChatAIDisplay final : public NoDisplay {
public:
    ChatAIDisplay() {
        width_ = 128;
        height_ = 64;
    }

    void SetStatus(const char* status) override {
        // OLED reads the live XiaoZhi device state in its renderer.
        (void)status;
    }

    void SetChatMessage(const char* role, const char* message) override {
        chatai::display::SetChatMessage(role, message);
    }

    void ClearChatMessages() override {
        chatai::display::ClearChatMessages();
    }
};

std::string PrintJson(cJSON* root) {
    if (root == nullptr) return "{}";
    char* serialized = cJSON_PrintUnformatted(root);
    std::string result = serialized != nullptr ? serialized : "{}";
    if (serialized != nullptr) cJSON_free(serialized);
    cJSON_Delete(root);
    return result;
}

class ChatAIBoard final : public Board {
public:
    std::string GetBoardType() override { return "wifi"; }
    AudioCodec* GetAudioCodec() override { return &audio_codec_; }

    Display* GetDisplay() override {
        static ChatAIDisplay display;
        return &display;
    }

    NetworkInterface* GetNetwork() override {
        static EspNetwork network;
        return &network;
    }

    void StartNetwork() override {
        ESP_LOGI(kTag, "Using ChatAI NetworkManager; Xiaozhi does not initialize Wi-Fi");
    }

    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            network_event_callback_ = std::move(callback);
        }
        chatai::network::NetworkManager::SetConnectionCallback(
            [this](bool connected, const std::string& ssid) {
                NetworkEventCallback callback_copy;
                {
                    std::lock_guard<std::mutex> lock(callback_mutex_);
                    callback_copy = network_event_callback_;
                }
                if (callback_copy) {
                    callback_copy(connected ? NetworkEvent::Connected : NetworkEvent::Disconnected,
                                  connected ? ssid : std::string{});
                }
            });
    }

    const char* GetNetworkStateIcon() override {
        return chatai::network::NetworkManager::IsConnected() ? "wifi" : "wifi_off";
    }

    void SetPowerSaveLevel(PowerSaveLevel level) override {
        (void)level;
        // ChatAI NetworkManager retains ownership of Wi-Fi power policy.
    }

    std::string GetBoardJson() override {
        cJSON* root = cJSON_CreateObject();
        if (root == nullptr) return "{}";
        cJSON_AddStringToObject(root, "type", "wifi");
        cJSON_AddStringToObject(root, "name", "ChatAI TPIOS");
        cJSON_AddStringToObject(root, "manufacturer", "TPIOS");
        cJSON_AddStringToObject(root, "model", "XH-S3E-AI V1.0");
        cJSON_AddStringToObject(root, "mac", SystemInfo::GetMacAddress().c_str());
        if (chatai::network::NetworkManager::IsConnected()) {
            cJSON_AddStringToObject(root, "ssid",
                                    chatai::network::NetworkManager::GetSsid().c_str());
            cJSON_AddNumberToObject(root, "rssi",
                                    chatai::network::NetworkManager::GetRssi());
            cJSON_AddStringToObject(root, "ip",
                                    chatai::network::NetworkManager::GetIpAddress().c_str());
        }
        return PrintJson(root);
    }

    std::string GetDeviceStatusJson() override {
        cJSON* root = cJSON_CreateObject();
        if (root == nullptr) return "{}";

        cJSON* speaker = cJSON_CreateObject();
        if (speaker != nullptr) {
            cJSON_AddNumberToObject(speaker, "volume", audio_codec_.output_volume());
            cJSON_AddItemToObject(root, "audio_speaker", speaker);
        }

        cJSON* screen = cJSON_CreateObject();
        if (screen != nullptr) {
            cJSON_AddNumberToObject(screen, "width", 128);
            cJSON_AddNumberToObject(screen, "height", 64);
            cJSON_AddBoolToObject(screen, "monochrome", true);
            cJSON_AddItemToObject(root, "screen", screen);
        }

        const bool connected = chatai::network::NetworkManager::IsConnected();
        const int rssi = connected ? chatai::network::NetworkManager::GetRssi() : 0;
        cJSON* network = cJSON_CreateObject();
        if (network != nullptr) {
            cJSON_AddStringToObject(network, "type", "wifi");
            cJSON_AddBoolToObject(network, "connected", connected);
            cJSON_AddStringToObject(network, "ssid",
                                    chatai::network::NetworkManager::GetSsid().c_str());
            cJSON_AddStringToObject(network, "ip",
                                    chatai::network::NetworkManager::GetIpAddress().c_str());
            cJSON_AddNumberToObject(network, "rssi", rssi);
            const char* signal = !connected ? "none" :
                (rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak"));
            cJSON_AddStringToObject(network, "signal", signal);
            cJSON_AddItemToObject(root, "network", network);
        }

        AddSystemStatus(root);
        return PrintJson(root);
    }

private:
    chatai::audio::ChatAIXiaozhiAudioCodec audio_codec_;
    std::mutex callback_mutex_;
    NetworkEventCallback network_event_callback_;
};
}  // namespace

DECLARE_BOARD(ChatAIBoard);
