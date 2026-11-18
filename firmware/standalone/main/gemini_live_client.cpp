#include "gemini_live_client.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>

#include "audio_manager.h"
#include "gemini_settings.h"
#include "network_manager.h"

namespace {
constexpr char kTag[] = "ChatAI-Gemini";
constexpr char kEndpoint[] =
    "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta."
    "GenerativeService.BidiGenerateContent?key=";
constexpr size_t kMaxIncomingMessage = 8192;
constexpr size_t kMaxOutputSamples = 2048;
constexpr size_t kMicChunkSamples = 320;  // 20 ms at 16 kHz.
constexpr uint32_t kRetryDelayMs = 5000;
constexpr UBaseType_t kAudioQueueLength = 8;

struct AudioChunk {
    uint32_t epoch;
    size_t sample_count;
    int16_t samples[kMaxOutputSamples];
};

std::atomic<bool> g_ready{false};
std::atomic<bool> g_task_started{false};
std::atomic<uint32_t> g_audio_epoch{0};
esp_websocket_client_handle_t g_client = nullptr;
QueueHandle_t g_audio_queue = nullptr;
std::string g_incoming_message;
bool g_receiving_text = false;
bool g_missing_key_reported = false;
std::mutex g_send_mutex;

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

void ClearAudioQueue() {
    if (g_audio_queue == nullptr) {
        return;
    }
    AudioChunk* chunk = nullptr;
    while (xQueueReceive(g_audio_queue, &chunk, 0) == pdTRUE) {
        std::free(chunk);
    }
}

void QueueServerAudio(const cJSON* inline_data) {
    const cJSON* mime = cJSON_GetObjectItemCaseSensitive(inline_data, "mimeType");
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(inline_data, "data");
    if (!cJSON_IsString(mime) || !cJSON_IsString(data) ||
        std::strncmp(mime->valuestring, "audio/pcm;rate=24000", 20) != 0 ||
        data->valuestring == nullptr || g_audio_queue == nullptr) {
        return;
    }

    const size_t encoded_size = std::strlen(data->valuestring);
    if (encoded_size == 0 || encoded_size > 6000) {
        ESP_LOGW(kTag, "Discarded oversized Gemini audio chunk");
        return;
    }

    auto* chunk = static_cast<AudioChunk*>(std::malloc(sizeof(AudioChunk)));
    if (chunk == nullptr) {
        ESP_LOGW(kTag, "Not enough memory for Gemini audio chunk");
        return;
    }

    size_t decoded_size = 0;
    const int result = mbedtls_base64_decode(
        reinterpret_cast<unsigned char*>(chunk->samples),
        sizeof(chunk->samples), &decoded_size,
        reinterpret_cast<const unsigned char*>(data->valuestring), encoded_size);
    if (result != 0 || decoded_size == 0 ||
        decoded_size % sizeof(int16_t) != 0 ||
        decoded_size > sizeof(chunk->samples)) {
        std::free(chunk);
        ESP_LOGW(kTag, "Discarded invalid Gemini PCM chunk");
        return;
    }

    chunk->epoch = g_audio_epoch.load();
    chunk->sample_count = decoded_size / sizeof(int16_t);
    if (xQueueSend(g_audio_queue, &chunk, 0) != pdTRUE) {
        std::free(chunk);
        ESP_LOGW(kTag, "Gemini playback queue full; dropping audio chunk");
    }
}

void HandleServerMessage(const std::string& message) {
    cJSON* root = cJSON_ParseWithLength(message.c_str(), message.size());
    if (root == nullptr) {
        ESP_LOGW(kTag, "Gemini sent an invalid JSON message");
        return;
    }

    const cJSON* setup_complete = cJSON_GetObjectItemCaseSensitive(root, "setupComplete");
    if (cJSON_IsObject(setup_complete)) {
        g_ready.store(true);
        ESP_LOGI(kTag, "Gemini Live setupComplete; audio session is ready");
    }

    const cJSON* server_content =
        cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    if (cJSON_IsObject(server_content)) {
        const cJSON* interrupted =
            cJSON_GetObjectItemCaseSensitive(server_content, "interrupted");
        if (cJSON_IsTrue(interrupted)) {
            g_audio_epoch.fetch_add(1);
            ClearAudioQueue();
            chatai::audio::EnableSpeaker(false);
            ESP_LOGI(kTag, "Gemini interrupted playback; queued audio cleared");
        }

        const cJSON* model_turn =
            cJSON_GetObjectItemCaseSensitive(server_content, "modelTurn");
        const cJSON* parts = cJSON_IsObject(model_turn)
            ? cJSON_GetObjectItemCaseSensitive(model_turn, "parts") : nullptr;
        if (cJSON_IsArray(parts)) {
            const int count = cJSON_GetArraySize(parts);
            for (int i = 0; i < count; ++i) {
                const cJSON* part = cJSON_GetArrayItem(parts, i);
                const cJSON* inline_data = cJSON_IsObject(part)
                    ? cJSON_GetObjectItemCaseSensitive(part, "inlineData") : nullptr;
                if (cJSON_IsObject(inline_data)) {
                    QueueServerAudio(inline_data);
                }
            }
        }
    }

    if (cJSON_GetObjectItemCaseSensitive(root, "error") != nullptr) {
        g_ready.store(false);
        ESP_LOGE(kTag, "Gemini rejected or terminated the Live session");
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

            std::lock_guard<std::mutex> lock(g_send_mutex);
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
            g_audio_epoch.fetch_add(1);
            ClearAudioQueue();
            ESP_LOGW(kTag, "Gemini Live WebSocket disconnected");
            break;

        case WEBSOCKET_EVENT_CLOSED:
            g_ready.store(false);
            g_audio_epoch.fetch_add(1);
            ClearAudioQueue();
            ESP_LOGW(kTag, "Gemini Live WebSocket closed; code=%d",
                     event != nullptr ? event->close_status_code : 0);
            break;

        default:
            break;
    }
}

void MicrophoneWorker(void*) {
    int16_t pcm[kMicChunkSamples]{};
    unsigned char encoded[900]{};
    char message[1200]{};
    bool microphone_enabled = false;
    bool microphone_error_reported = false;

    while (true) {
        if (!g_ready.load() || g_client == nullptr ||
            !esp_websocket_client_is_connected(g_client)) {
            if (microphone_enabled) {
                chatai::audio::EnableMicrophone(false);
                microphone_enabled = false;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!microphone_enabled) {
            const esp_err_t enable_result = chatai::audio::EnableMicrophone(true);
            if (enable_result != ESP_OK) {
                if (!microphone_error_reported) {
                    ESP_LOGE(kTag, "Could not enable microphone: %s",
                             esp_err_to_name(enable_result));
                    microphone_error_reported = true;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            microphone_enabled = true;
            microphone_error_reported = false;
            ESP_LOGI(kTag, "Microphone streaming started: PCM16 mono 16 kHz");
        }

        size_t samples_read = 0;
        const esp_err_t read_result = chatai::audio::ReadMicrophonePcm16(
            pcm, kMicChunkSamples, &samples_read, 100);
        if (read_result == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (read_result != ESP_OK || samples_read == 0) {
            ESP_LOGW(kTag, "Microphone read failed: %s",
                     esp_err_to_name(read_result));
            chatai::audio::EnableMicrophone(false);
            microphone_enabled = false;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        size_t encoded_size = 0;
        const int encode_result = mbedtls_base64_encode(
            encoded, sizeof(encoded), &encoded_size,
            reinterpret_cast<const unsigned char*>(pcm),
            samples_read * sizeof(int16_t));
        if (encode_result != 0 || encoded_size >= sizeof(encoded)) {
            ESP_LOGE(kTag, "Could not encode microphone PCM chunk");
            continue;
        }
        encoded[encoded_size] = '\0';

        const int message_size = std::snprintf(
            message, sizeof(message),
            "{\"realtimeInput\":{\"audio\":{"
            "\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}",
            reinterpret_cast<const char*>(encoded));
        if (message_size <= 0 || message_size >= static_cast<int>(sizeof(message))) {
            ESP_LOGE(kTag, "Microphone audio message exceeded its buffer");
            continue;
        }

        std::lock_guard<std::mutex> lock(g_send_mutex);
        if (g_ready.load() && g_client != nullptr &&
            esp_websocket_client_is_connected(g_client)) {
            const int sent = esp_websocket_client_send_text(
                g_client, message, message_size, pdMS_TO_TICKS(1000));
            if (sent != message_size) {
                ESP_LOGW(kTag, "Microphone audio send was incomplete");
            }
        }
    }
}

void SpeakerWorker(void*) {
    AudioChunk* chunk = nullptr;
    bool speaker_enabled = false;

    while (true) {
        if (g_audio_queue == nullptr ||
            xQueueReceive(g_audio_queue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (speaker_enabled && g_audio_queue != nullptr &&
                uxQueueMessagesWaiting(g_audio_queue) == 0) {
                chatai::audio::EnableSpeaker(false);
                speaker_enabled = false;
            }
            continue;
        }

        if (chunk == nullptr) {
            continue;
        }
        if (chunk->epoch != g_audio_epoch.load() || !g_ready.load()) {
            std::free(chunk);
            chunk = nullptr;
            continue;
        }

        if (!speaker_enabled) {
            const esp_err_t enable_result = chatai::audio::EnableSpeaker(true);
            if (enable_result != ESP_OK) {
                ESP_LOGE(kTag, "Could not enable speaker: %s",
                         esp_err_to_name(enable_result));
                std::free(chunk);
                chunk = nullptr;
                continue;
            }
            speaker_enabled = true;
        }

        size_t samples_written = 0;
        const esp_err_t write_result = chatai::audio::WriteSpeakerPcm16(
            chunk->samples, chunk->sample_count, &samples_written, 100);
        if (write_result != ESP_OK || samples_written != chunk->sample_count) {
            ESP_LOGW(kTag, "Speaker write incomplete: %s",
                     esp_err_to_name(write_result));
        }
        std::free(chunk);
        chunk = nullptr;
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

    g_audio_queue = xQueueCreate(kAudioQueueLength, sizeof(AudioChunk*));
    if (g_audio_queue == nullptr) {
        g_task_started.store(false);
        ESP_LOGE(kTag, "Could not create Gemini playback queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t result = xTaskCreate(
        SpeakerWorker, "chatai_spk", 4096, nullptr, 5, nullptr);
    if (result == pdPASS) {
        result = xTaskCreate(
            MicrophoneWorker, "chatai_mic", 6144, nullptr, 5, nullptr);
    }
    if (result == pdPASS) {
        result = xTaskCreate(
            GeminiWorker, "chatai_gemini", 6144, nullptr, 5, nullptr);
    }
    if (result != pdPASS) {
        g_task_started.store(false);
        ESP_LOGE(kTag, "Could not create Gemini audio/network worker tasks");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "Gemini Live audio workers initialized");
    return ESP_OK;
}

bool IsReady() {
    return g_ready.load();
}

}  // namespace chatai::gemini
