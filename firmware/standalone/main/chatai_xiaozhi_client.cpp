#include "chatai_xiaozhi_client.h"

#include "chatai_xiaozhi_audio_runtime.h"
#include "chatai_xiaozhi_mqtt.h"
#include "chatai_xiaozhi_ota.h"
#include "diagnostic_manager.h"
#include "display_manager.h"
#include "gemini_live_client.h"
#include "network_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>
#include <sys/time.h>

#include <arpa/inet.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <psa/crypto.h>

namespace chatai::xiaozhi {
namespace {

constexpr char kTag[] = "XiaozhiClient";
constexpr uint32_t kRetryDelayMs = 15000;
constexpr uint32_t kHelloTimeoutMs = 12000;
constexpr size_t kUdpHeaderSize = 16;
constexpr size_t kMaxUdpPacketSize = 1500;
constexpr EventBits_t kHelloReceived = 1 << 0;
constexpr EventBits_t kHelloFailed = 1 << 1;
constexpr EventBits_t kServerEnded = 1 << 2;
constexpr EventBits_t kUdpRxExited = 1 << 3;
constexpr EventBits_t kUdpTxExited = 1 << 4;

bool DecodeHex(const char* text, std::vector<uint8_t>* bytes) {
    if (text == nullptr || bytes == nullptr) return false;
    const size_t length = std::strlen(text);
    if (length == 0 || (length & 1U) != 0) return false;
    bytes->clear();
    bytes->reserve(length / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < length; i += 2) {
        const int high = nibble(text[i]);
        const int low = nibble(text[i + 1]);
        if (high < 0 || low < 0) {
            bytes->clear();
            return false;
        }
        bytes->push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return true;
}

bool ReadString(const cJSON* object, const char* name, std::string* value) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == nullptr ||
        item->valuestring[0] == '\0') return false;
    *value = item->valuestring;
    return true;
}

class XiaozhiClient final {
public:
    esp_err_t Initialize() {
        if (initialized_.load()) return ESP_OK;
        events_ = xEventGroupCreate();
        if (events_ == nullptr) return ESP_ERR_NO_MEM;
        const esp_err_t audio_result = audio_.Initialize();
        if (audio_result != ESP_OK) {
            vEventGroupDelete(events_);
            events_ = nullptr;
            return audio_result;
        }
        initialized_.store(true);
        if (xTaskCreate(WorkerTaskEntry, "xiaozhi_net", 6144, this, 5,
                        &worker_task_) != pdPASS) {
            initialized_.store(false);
            audio_.Stop();
            vEventGroupDelete(events_);
            events_ = nullptr;
            worker_task_ = nullptr;
            return ESP_ERR_NO_MEM;
        }
        chatai::diagnostics::Add("XIAOZHI", "STARTING",
                                 "OTA discovery and MQTT+UDP session manager started");
        return ESP_OK;
    }

    bool IsReady() const { return session_ready_.load(); }

    void SetChatEnabled(bool enabled) {
        chat_enabled_.store(enabled);
        UpdateListeningState();
    }

    bool IsChatEnabled() const { return chat_enabled_.load(); }

    void SetPushToTalk(bool pressed) {
        push_to_talk_.store(pressed);
        UpdateListeningState();
    }

    bool IsListening() const {
        return chat_enabled_.load() || push_to_talk_.load();
    }

private:
    static void WorkerTaskEntry(void* context) {
        static_cast<XiaozhiClient*>(context)->WorkerTask();
        vTaskDelete(nullptr);
    }

    static void UdpReceiveTaskEntry(void* context) {
        auto* self = static_cast<XiaozhiClient*>(context);
        self->UdpReceiveTask();
        xEventGroupSetBits(self->events_, kUdpRxExited);
        vTaskDelete(nullptr);
    }

    static void UdpTransmitTaskEntry(void* context) {
        auto* self = static_cast<XiaozhiClient*>(context);
        self->UdpTransmitTask();
        xEventGroupSetBits(self->events_, kUdpTxExited);
        vTaskDelete(nullptr);
    }

    void WorkerTask() {
        while (true) {
            if (!chatai::network::NetworkManager::IsConnected()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            ServerConfig config;
            const esp_err_t ota_result = FetchServerConfig(&config);
            if (ota_result == ESP_ERR_NOT_FINISHED && config.activation_required) {
                char detail[192]{};
                std::snprintf(detail, sizeof(detail), "activation_code=%s challenge=%s",
                              config.activation_code.c_str(),
                              config.activation_challenge.c_str());
                chatai::diagnostics::Add("XIAOZHI_ACTIVATION", "REQUIRED", detail);
                chatai::display::Notify("XIAOZHI",
                    config.activation_message.empty()
                        ? "Cần kích hoạt thiết bị Xiaozhi"
                        : config.activation_message.c_str(), 8000);
                vTaskDelay(pdMS_TO_TICKS(60000));
                continue;
            }
            if (ota_result != ESP_OK) {
                chatai::diagnostics::Add("XIAOZHI_OTA", "FAILED",
                                         esp_err_to_name(ota_result));
                vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
                continue;
            }
            if (config.transport != Transport::kMqttUdp) {
                chatai::diagnostics::Add("XIAOZHI", "UNSUPPORTED",
                    "Server selected WebSocket; MQTT+UDP session is not started");
                vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
                continue;
            }
            if (config.mqtt_subscribe_topic.empty()) {
                chatai::diagnostics::Add("XIAOZHI_MQTT", "FAILED",
                    "OTA response omitted subscribe_topic; server hello cannot be received");
                vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
                continue;
            }

            xEventGroupClearBits(events_, kHelloReceived | kHelloFailed |
                                         kServerEnded | kUdpRxExited | kUdpTxExited);
            esp_err_t result = mqtt_.Start(config,
                [this](const std::string& topic, const std::string& payload) {
                    OnMqttMessage(topic, payload);
                });
            if (result == ESP_OK) {
                result = SendHello();
            }
            if (result == ESP_OK) {
                const EventBits_t bits = xEventGroupWaitBits(
                    events_, kHelloReceived | kHelloFailed, pdFALSE, pdFALSE,
                    pdMS_TO_TICKS(kHelloTimeoutMs));
                if ((bits & kHelloReceived) == 0) {
                    result = (bits & kHelloFailed) ? ESP_FAIL : ESP_ERR_TIMEOUT;
                }
            }

            if (result == ESP_OK) {
                chatai::diagnostics::Add("XIAOZHI", "READY",
                                         "MQTT control and encrypted UDP audio channel ready");
                while (session_ready_.load() && mqtt_.IsConnected() &&
                       chatai::network::NetworkManager::IsConnected()) {
                    const EventBits_t bits = xEventGroupWaitBits(
                        events_, kServerEnded, pdTRUE, pdFALSE,
                        pdMS_TO_TICKS(500));
                    if (bits & kServerEnded) break;
                }
            } else {
                char detail[96]{};
                std::snprintf(detail, sizeof(detail), "session setup failed: %s",
                              esp_err_to_name(result));
                chatai::diagnostics::Add("XIAOZHI", "FAILED", detail);
            }

            StopAudioChannel();
            mqtt_.Stop();
            chat_enabled_.store(false);
            push_to_talk_.store(false);
            last_listening_.store(false);
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
        }
    }

    esp_err_t SendHello() {
        cJSON* root = cJSON_CreateObject();
        if (root == nullptr) return ESP_ERR_NO_MEM;
        cJSON_AddStringToObject(root, "type", "hello");
        cJSON_AddNumberToObject(root, "version", 3);
        cJSON_AddStringToObject(root, "transport", "udp");

        cJSON* features = cJSON_AddObjectToObject(root, "features");
        if (features == nullptr) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddBoolToObject(features, "mcp", false);
        cJSON_AddBoolToObject(features, "aec", false);
        cJSON_AddBoolToObject(features, "glyph_push", false);
        cJSON_AddBoolToObject(features, "udp_probe", false);

        cJSON* audio_params = cJSON_AddObjectToObject(root, "audio_params");
        if (audio_params == nullptr) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(audio_params, "format", "opus");
        cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
        cJSON_AddNumberToObject(audio_params, "channels", 1);
        cJSON_AddNumberToObject(audio_params, "frame_duration", 60);

        char* json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (json == nullptr) return ESP_ERR_NO_MEM;
        const esp_err_t result = mqtt_.Publish(json);
        cJSON_free(json);
        return result;
    }

    void OnMqttMessage(const std::string&, const std::string& payload) {
        cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
        if (root == nullptr) return;
        std::string type;
        if (!ReadString(root, "type", &type)) {
            cJSON_Delete(root);
            return;
        }

        if (type == "hello") {
            const esp_err_t result = ConfigureAudioChannel(root);
            xEventGroupSetBits(events_, result == ESP_OK
                ? kHelloReceived : kHelloFailed);
        } else if (type == "goodbye") {
            std::string received_session;
            std::string current_session;
            {
                std::lock_guard<std::mutex> lock(channel_mutex_);
                current_session = session_id_;
            }
            if (ReadString(root, "session_id", &received_session) &&
                received_session == current_session) {
                xEventGroupSetBits(events_, kServerEnded);
            }
        } else if (type == "alert") {
            std::string message;
            if (ReadString(root, "message", &message)) {
                chatai::display::Notify("XIAOZHI", message.c_str(), 5000);
            }
        }
        cJSON_Delete(root);
    }

    esp_err_t ConfigureAudioChannel(const cJSON* root) {
        std::string transport;
        std::string session_id;
        if (!ReadString(root, "transport", &transport) || transport != "udp" ||
            !ReadString(root, "session_id", &session_id)) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        const cJSON* audio_params = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
        if (!cJSON_IsObject(audio_params)) return ESP_ERR_INVALID_RESPONSE;
        const cJSON* sample_rate_item =
            cJSON_GetObjectItemCaseSensitive(audio_params, "sample_rate");
        const cJSON* frame_duration_item =
            cJSON_GetObjectItemCaseSensitive(audio_params, "frame_duration");
        if (!cJSON_IsNumber(sample_rate_item) ||
            !cJSON_IsNumber(frame_duration_item)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint32_t sample_rate =
            static_cast<uint32_t>(sample_rate_item->valueint);
        const uint32_t frame_duration =
            static_cast<uint32_t>(frame_duration_item->valueint);
        if (sample_rate != 24000 || frame_duration != 60) {
            ESP_LOGE(kTag, "Unsupported negotiated audio format: %lu Hz / %lu ms",
                     static_cast<unsigned long>(sample_rate),
                     static_cast<unsigned long>(frame_duration));
            return ESP_ERR_NOT_SUPPORTED;
        }

        const cJSON* udp = cJSON_GetObjectItemCaseSensitive(root, "udp");
        std::string server;
        if (!cJSON_IsObject(udp) ||
            !ReadString(udp, "server", &server)) return ESP_ERR_INVALID_RESPONSE;
        const cJSON* port_item = cJSON_GetObjectItemCaseSensitive(udp, "port");
        if (!cJSON_IsNumber(port_item) || port_item->valueint <= 0 ||
            port_item->valueint > 65535) return ESP_ERR_INVALID_RESPONSE;

        std::vector<uint8_t> key;
        std::vector<uint8_t> nonce;
        const cJSON* key_item = cJSON_GetObjectItemCaseSensitive(udp, "key");
        const cJSON* nonce_item = cJSON_GetObjectItemCaseSensitive(udp, "nonce");
        if (!cJSON_IsString(key_item) || !cJSON_IsString(nonce_item) ||
            !DecodeHex(key_item->valuestring, &key) ||
            !DecodeHex(nonce_item->valuestring, &nonce) ||
            key.size() != 16 || nonce.size() != kUdpHeaderSize) {
            return ESP_ERR_INVALID_RESPONSE;
        }

        const int new_socket = OpenUdpSocket(server, port_item->valueint);
        if (new_socket < 0) return ESP_FAIL;

        psa_status_t crypto_result = psa_crypto_init();
        if (crypto_result != PSA_SUCCESS) {
            close(new_socket);
            return ESP_FAIL;
        }
        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&attributes, 128);
        psa_set_key_usage_flags(&attributes,
            PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
        psa_set_key_algorithm(&attributes, PSA_ALG_CTR);
        psa_key_id_t new_key = PSA_KEY_ID_NULL;
        crypto_result = psa_import_key(&attributes, key.data(), key.size(), &new_key);
        psa_reset_key_attributes(&attributes);
        if (crypto_result != PSA_SUCCESS) {
            close(new_socket);
            return ESP_FAIL;
        }

        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            socket_fd_ = new_socket;
            aes_key_id_ = new_key;
            std::copy(nonce.begin(), nonce.end(), udp_nonce_.begin());
            udp_server_ = server;
            udp_port_ = port_item->valueint;
            session_id_ = std::move(session_id);
            sample_rate_ = sample_rate;
            frame_duration_ms_ = frame_duration;
            local_sequence_ = 0;
            remote_sequence_ = 0;
        }

        chatai::gemini::SetChatEnabled(false);
        chatai::gemini::SetPushToTalk(false);
        if (chatai::gemini::IsSpeaking()) chatai::gemini::AbortSpeaking();

        esp_err_t result = audio_.Start();
        if (result != ESP_OK) {
            ReleaseUdpResources();
            return result;
        }
        udp_running_.store(true);
        if (xTaskCreate(UdpReceiveTaskEntry, "xiaozhi_udp_rx", 4096,
                        this, 5, &udp_rx_task_) != pdPASS) {
            udp_running_.store(false);
            audio_.Stop();
            ReleaseUdpResources();
            return ESP_ERR_NO_MEM;
        }
        if (xTaskCreate(UdpTransmitTaskEntry, "xiaozhi_udp_tx", 4096,
                        this, 5, &udp_tx_task_) != pdPASS) {
            udp_running_.store(false);
            // Receive task exits on its bounded socket timeout.
            xEventGroupWaitBits(events_, kUdpRxExited, pdTRUE, pdTRUE, portMAX_DELAY);
            udp_rx_task_ = nullptr;
            audio_.Stop();
            ReleaseUdpResources();
            return ESP_ERR_NO_MEM;
        }
        session_ready_.store(true);
        UpdateListeningState();
        return ESP_OK;
    }

    int OpenUdpSocket(const std::string& host, int port) {
        char port_text[8]{};
        std::snprintf(port_text, sizeof(port_text), "%d", port);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), port_text, &hints, &result) != 0 ||
            result == nullptr) return -1;

        int fd = -1;
        for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
            fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
            if (fd < 0) continue;
            if (connect(fd, item->ai_addr, item->ai_addrlen) == 0) break;
            close(fd);
            fd = -1;
        }
        freeaddrinfo(result);
        if (fd < 0) return -1;

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        return fd;
    }

    bool Crypt(const uint8_t* input, size_t size, const uint8_t* nonce,
               uint8_t* output) {
        if (input == nullptr || nonce == nullptr || output == nullptr) return false;
        psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
        psa_status_t result;
        {
            std::lock_guard<std::mutex> lock(crypto_mutex_);
            if (aes_key_id_ == PSA_KEY_ID_NULL) return false;
            result = psa_cipher_encrypt_setup(&operation, aes_key_id_, PSA_ALG_CTR);
        }
        if (result == PSA_SUCCESS) result = psa_cipher_set_iv(&operation, nonce, 16);
        size_t output_size = 0;
        if (result == PSA_SUCCESS) {
            result = psa_cipher_update(&operation, input, size, output, size, &output_size);
        }
        uint8_t tail[16]{};
        size_t tail_size = 0;
        if (result == PSA_SUCCESS) {
            result = psa_cipher_finish(&operation, tail, sizeof(tail), &tail_size);
        }
        psa_cipher_abort(&operation);
        return result == PSA_SUCCESS && output_size == size && tail_size == 0;
    }

    void UdpReceiveTask() {
        std::array<uint8_t, kMaxUdpPacketSize> packet{};
        while (udp_running_.load()) {
            int fd;
            {
                std::lock_guard<std::mutex> lock(channel_mutex_);
                fd = socket_fd_;
            }
            if (fd < 0) break;
            const int received = recv(fd, packet.data(), packet.size(), 0);
            if (received < 0) {
                continue;
            }
            if (received < static_cast<int>(kUdpHeaderSize) || packet[0] != 0x01) {
                continue;
            }

            uint16_t payload_length = 0;
            uint32_t timestamp = 0;
            uint32_t sequence = 0;
            std::memcpy(&payload_length, packet.data() + 2, sizeof(payload_length));
            std::memcpy(&timestamp, packet.data() + 8, sizeof(timestamp));
            std::memcpy(&sequence, packet.data() + 12, sizeof(sequence));
            payload_length = ntohs(payload_length);
            timestamp = ntohl(timestamp);
            sequence = ntohl(sequence);
            if (received != static_cast<int>(kUdpHeaderSize + payload_length) ||
                payload_length == 0) continue;

            {
                std::lock_guard<std::mutex> lock(channel_mutex_);
                if (sequence <= remote_sequence_) continue;
            }

            std::vector<uint8_t> decoded(payload_length);
            if (!Crypt(packet.data() + kUdpHeaderSize, payload_length,
                       packet.data(), decoded.data())) continue;

            {
                std::lock_guard<std::mutex> lock(channel_mutex_);
                if (sequence <= remote_sequence_) continue;
                remote_sequence_ = sequence;
            }
            const esp_err_t result = audio_.SubmitDownlinkFrame(
                decoded.data(), decoded.size(), sample_rate_, frame_duration_ms_);
            if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(kTag, "Dropped downlink Opus frame: %s",
                         esp_err_to_name(result));
            }
            (void)timestamp;
        }
    }

    void UdpTransmitTask() {
        while (udp_running_.load()) {
            chatai::audio::XiaozhiUplinkFrame frame;
            if (!audio_.WaitPopUplinkFrame(frame, 100)) continue;
            if (!udp_running_.load() || !IsListening()) continue;

            int fd;
            std::array<uint8_t, kUdpHeaderSize> header{};
            uint32_t sequence;
            {
                std::lock_guard<std::mutex> lock(channel_mutex_);
                fd = socket_fd_;
                header = udp_nonce_;
                sequence = ++local_sequence_;
            }
            if (fd < 0 || frame.payload.empty() ||
                frame.payload.size() > UINT16_MAX) continue;

            const uint16_t payload_length = htons(
                static_cast<uint16_t>(frame.payload.size()));
            const uint32_t timestamp = htonl(frame.timestamp_ms);
            const uint32_t network_sequence = htonl(sequence);
            std::memcpy(header.data() + 2, &payload_length, sizeof(payload_length));
            std::memcpy(header.data() + 8, &timestamp, sizeof(timestamp));
            std::memcpy(header.data() + 12, &network_sequence,
                        sizeof(network_sequence));

            std::vector<uint8_t> packet(kUdpHeaderSize + frame.payload.size());
            std::copy(header.begin(), header.end(), packet.begin());
            if (!Crypt(frame.payload.data(), frame.payload.size(), header.data(),
                       packet.data() + kUdpHeaderSize)) continue;
            const int sent = send(fd, packet.data(), packet.size(), 0);
            if (sent != static_cast<int>(packet.size())) {
                ESP_LOGW(kTag, "UDP audio send failed");
            }
        }
    }

    void UpdateListeningState() {
        const bool listening = IsListening();
        if (!session_ready_.load()) return;
        const bool previous = last_listening_.load();
        if (previous == listening) {
            audio_.SetCaptureEnabled(listening);
            return;
        }
        if (!listening) audio_.SetCaptureEnabled(false);

        cJSON* root = cJSON_CreateObject();
        if (root == nullptr) return;
        std::string session_id;
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            session_id = session_id_;
        }
        cJSON_AddStringToObject(root, "session_id", session_id.c_str());
        cJSON_AddStringToObject(root, "type", "listen");
        cJSON_AddStringToObject(root, "state", listening ? "start" : "stop");
        if (listening) {
            cJSON_AddStringToObject(root, "mode",
                chat_enabled_.load() ? "auto" : "manual");
        }
        char* json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (json == nullptr) return;
        const esp_err_t result = mqtt_.Publish(json);
        cJSON_free(json);
        if (result == ESP_OK) {
            last_listening_.store(listening);
            if (listening) audio_.SetCaptureEnabled(true);
        } else {
            ESP_LOGW(kTag, "Could not send listen state to Xiaozhi server");
        }
    }

    void StopAudioChannel() {
        session_ready_.store(false);
        last_listening_.store(false);
        audio_.SetCaptureEnabled(false);
        udp_running_.store(false);
        // recv() uses a short timeout; let the UDP task observe udp_running_ before close.
        audio_.Stop();
        const bool rx_started = udp_rx_task_ != nullptr;
        const bool tx_started = udp_tx_task_ != nullptr;
        EventBits_t wait_bits = 0;
        if (rx_started) wait_bits |= kUdpRxExited;
        if (tx_started) wait_bits |= kUdpTxExited;
        if (wait_bits != 0) {
            xEventGroupWaitBits(events_, wait_bits, pdTRUE, pdTRUE, portMAX_DELAY);
        }
        udp_rx_task_ = nullptr;
        udp_tx_task_ = nullptr;
        ReleaseUdpResources();
    }

    void ReleaseUdpResources() {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
        if (aes_key_id_ != PSA_KEY_ID_NULL) {
            psa_destroy_key(aes_key_id_);
            aes_key_id_ = PSA_KEY_ID_NULL;
        }
        udp_nonce_.fill(0);
        session_id_.clear();
        udp_server_.clear();
        udp_port_ = 0;
    }

    static void SetWorkerDiagnostic(const char* stage, const char* result,
                                    const char* detail) {
        chatai::diagnostics::Add(stage, result, detail);
    }

    chatai::audio::XiaozhiAudioRuntime audio_;
    MqttTransport mqtt_;
    EventGroupHandle_t events_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    TaskHandle_t udp_rx_task_ = nullptr;
    TaskHandle_t udp_tx_task_ = nullptr;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> session_ready_{false};
    std::atomic<bool> udp_running_{false};
    std::atomic<bool> chat_enabled_{false};
    std::atomic<bool> push_to_talk_{false};
    std::atomic<bool> last_listening_{false};
    std::mutex channel_mutex_;
    std::mutex crypto_mutex_;
    int socket_fd_ = -1;
    psa_key_id_t aes_key_id_ = PSA_KEY_ID_NULL;
    std::array<uint8_t, kUdpHeaderSize> udp_nonce_{};
    std::string udp_server_;
    int udp_port_ = 0;
    std::string session_id_;
    uint32_t sample_rate_ = 24000;
    uint32_t frame_duration_ms_ = 60;
    uint32_t local_sequence_ = 0;
    uint32_t remote_sequence_ = 0;
};

XiaozhiClient g_client;

}  // namespace

esp_err_t Initialize() { return g_client.Initialize(); }
bool IsReady() { return g_client.IsReady(); }
void SetChatEnabled(bool enabled) { g_client.SetChatEnabled(enabled); }
bool IsChatEnabled() { return g_client.IsChatEnabled(); }
void SetPushToTalk(bool pressed) { g_client.SetPushToTalk(pressed); }
bool IsListening() { return g_client.IsListening(); }

}  // namespace chatai::xiaozhi
