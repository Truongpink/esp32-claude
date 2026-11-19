#include "gemini_live_client.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_websocket_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>

#include "audio_manager.h"
#include "gemini_settings.h"
#include "network_manager.h"
#include "diagnostic_manager.h"
#include "display_manager.h"

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
std::atomic<bool> g_speaking{false};
std::atomic<bool> g_error{false};
std::atomic<bool> g_waiting_setup{false};
std::atomic<uint32_t> g_setup_started_tick{0};
std::atomic<bool> g_task_started{false};
std::atomic<bool> g_chat_enabled{false};
std::atomic<bool> g_push_to_talk{false};
std::atomic<bool> g_restart_requested{false};
bool g_sntp_initialized = false;
std::atomic<uint32_t> g_audio_epoch{0};
std::atomic<bool> g_audio_response_reported{false};
std::atomic<bool> g_audio_input_reported{false};
std::atomic<bool> g_speaker_started_reported{false};
std::atomic<bool> g_auth_reported{false};
std::atomic<bool> g_tls_attempt_reported{false};
esp_websocket_client_handle_t g_client = nullptr;
QueueHandle_t g_audio_queue = nullptr;
std::string g_incoming_message;
bool g_receiving_text = false;
bool g_missing_key_reported = false;
std::mutex g_send_mutex;

bool ShouldListen() {
    return g_chat_enabled.load() || g_push_to_talk.load();
}

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

bool EnsureSystemTime() {
    constexpr std::time_t kMinimumValidTime = 1700000000;
    if (std::time(nullptr) >= kMinimumValidTime) { chatai::diagnostics::Add("TIME", "OK", "system clock valid"); return true; }

    if (!g_sntp_initialized) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        config.start = false;
        const esp_err_t result = esp_netif_sntp_init(&config);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "Could not initialize SNTP: %s", esp_err_to_name(result));
            chatai::diagnostics::Add("TIME", "FAIL", esp_err_to_name(result));
            return false;
        }
        g_sntp_initialized = true;
    }

    const esp_err_t start_result = esp_netif_sntp_start();
    if (start_result != ESP_OK) {
        ESP_LOGW(kTag, "Could not start SNTP: %s", esp_err_to_name(start_result));
        chatai::diagnostics::Add("TIME", "FAIL", esp_err_to_name(start_result));
        return false;
    }

    const esp_err_t sync_result =
        esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    if (sync_result != ESP_OK && std::time(nullptr) < kMinimumValidTime) {
        ESP_LOGW(kTag, "Waiting for system time synchronization before Gemini TLS: %s",
                 esp_err_to_name(sync_result));
        chatai::diagnostics::Add("TIME", "FAIL", esp_err_to_name(sync_result));
        return false;
    }

    ESP_LOGI(kTag, "System time synchronized; Gemini TLS certificate validation can proceed");
    chatai::diagnostics::Add("TIME", "OK", "SNTP synchronized");
    return true;
}

const char kSetupMessage[] =
    "{\"setup\":{"
      "\"model\":\"models/gemini-3.8-live\","
      "\"responseModalities\":[\"AUDIO\"],"
      "\"systemInstruction\":{"
        "\"parts\":[{"
          "\"text\":\"Bạn là trợ lý ChatAI TPIOS. Hãy giao tiếp tự nhiên, thân thiện "
          "và rõ ràng bằng tiếng Việt. Chỉ trả lời bằng giọng nói trong phiên hội thoại "
          "thời gian thực.\""
        "}]"
      "}"
    "}}";

void ClearAudioQueue() {
    if (g_audio_queue == nullptr) return;
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
        data->valuestring == nullptr || g_audio_queue == nullptr) return;

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
        reinterpret_cast<unsigned char*>(chunk->samples), sizeof(chunk->samples),
        &decoded_size, reinterpret_cast<const unsigned char*>(data->valuestring),
        encoded_size);
    if (result != 0 || decoded_size == 0 ||
        decoded_size % sizeof(int16_t) != 0 || decoded_size > sizeof(chunk->samples)) {
        std::free(chunk);
        ESP_LOGW(kTag, "Discarded invalid Gemini PCM chunk");
        return;
    }

    chunk->epoch = g_audio_epoch.load();
    chunk->sample_count = decoded_size / sizeof(int16_t);
    if (xQueueSend(g_audio_queue, &chunk, 0) != pdTRUE) {
        std::free(chunk);
        ESP_LOGW(kTag, "Gemini playback queue full; dropping audio chunk");
    } else {
        if (!g_audio_response_reported.exchange(true))
            chatai::diagnostics::Add("AUDIO_OUT", "OK", "Gemini PCM16 24000 Hz response received");
        g_speaking.store(true);
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
        g_waiting_setup.store(false);
        g_ready.store(true);
        g_error.store(false);
        ESP_LOGI(kTag, "Gemini Live setupComplete; audio session is ready");
        chatai::diagnostics::Add("SETUP", "OK", "setupComplete received; Gemini READY");
    }

    const cJSON* server_content =
        cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    if (cJSON_IsObject(server_content)) {
        const cJSON* interrupted =
            cJSON_GetObjectItemCaseSensitive(server_content, "interrupted");
        if (cJSON_IsTrue(interrupted)) {
            g_audio_epoch.fetch_add(1);
            ClearAudioQueue();
            g_speaking.store(false);
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
                if (cJSON_IsObject(inline_data)) QueueServerAudio(inline_data);
            }
        }
    }

    const cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (error != nullptr) {
        g_waiting_setup.store(false);
        g_ready.store(false);
        g_speaking.store(false);
        g_error.store(true);

        const cJSON* status = cJSON_IsObject(error)
            ? cJSON_GetObjectItemCaseSensitive(error, "status") : nullptr;
        const cJSON* message_text = cJSON_IsObject(error)
            ? cJSON_GetObjectItemCaseSensitive(error, "message") : nullptr;
        if (cJSON_IsString(status) && cJSON_IsString(message_text)) {
            const int status_length = static_cast<int>(
                std::min<size_t>(std::strlen(status->valuestring), 64));
            const int message_length = static_cast<int>(
                std::min<size_t>(std::strlen(message_text->valuestring), 160));
            ESP_LOGE(kTag, "Gemini API error: status=%.*s message=%.*s",
                     status_length, status->valuestring,
                     message_length, message_text->valuestring);
            char detail[240]{};
            std::snprintf(detail, sizeof(detail), "status=%.*s message=%.*s",
                          status_length, status->valuestring,
                          message_length, message_text->valuestring);
            chatai::diagnostics::Add("GEMINI_API", "FAIL", detail);
            chatai::display::Notify("GEMINI API", "Loi phan hoi", 4000);
        } else {
            ESP_LOGE(kTag, "Gemini rejected or terminated the Live session");
            chatai::diagnostics::Add("GEMINI_API", "FAIL", "error response without status/message");
        }
    }

    cJSON_Delete(root);
}

void WebSocketEventHandler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
    auto* event = static_cast<esp_websocket_event_data_t*>(event_data);

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED: {
            g_ready.store(false);
            g_waiting_setup.store(true);
            g_setup_started_tick.store(static_cast<uint32_t>(xTaskGetTickCount()));
            g_speaking.store(false);
            g_error.store(false);
            g_incoming_message.clear();
            g_receiving_text = false;
            ESP_LOGI(kTag, "Gemini Live WebSocket connected; sending setup");
            chatai::diagnostics::Add("WS", "OK", "WebSocket handshake completed");
            chatai::diagnostics::Add("SETUP", "INFO", "setup request sending");

            std::lock_guard<std::mutex> lock(g_send_mutex);
            const int sent = g_client != nullptr
                ? esp_websocket_client_send_text(
                    g_client, kSetupMessage, sizeof(kSetupMessage) - 1, pdMS_TO_TICKS(5000))
                : -1;
            if (sent == static_cast<int>(sizeof(kSetupMessage) - 1)) {
                chatai::diagnostics::Add("SETUP", "INFO", "setup message sent; waiting for setupComplete");
            }
            if (sent != static_cast<int>(sizeof(kSetupMessage) - 1)) {
                g_error.store(true);
                ESP_LOGE(kTag, "Could not send Gemini Live setup message");
                chatai::diagnostics::Add("SETUP", "FAIL", "WebSocket send setup failed");
                chatai::display::Notify("GEMINI", "Loi gui setup", 4000);
            }
            break;
        }

        case WEBSOCKET_EVENT_DATA:
            if (event == nullptr || event->data_ptr == nullptr ||
                event->data_len <= 0 || event->payload_len <= 0 ||
                event->payload_len > static_cast<int>(kMaxIncomingMessage) ||
                event->payload_offset < 0) break;

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

            g_incoming_message.append(event->data_ptr, static_cast<size_t>(event->data_len));
            if (event->fin) {
                HandleServerMessage(g_incoming_message);
                g_incoming_message.clear();
                g_receiving_text = false;
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            g_waiting_setup.store(false);
            g_ready.store(false);
            g_speaking.store(false);
            g_error.store(true);
            if (event != nullptr) {
                chatai::diagnostics::Add("WS", "FAIL", "transport event reported");
                chatai::display::Notify("GEMINI", "Loi ket noi WSS", 4000);
                if (event->error_handle.esp_tls_last_esp_err != ESP_OK ||
                    event->error_handle.esp_tls_stack_err != 0 ||
                    event->error_handle.esp_tls_cert_verify_flags != 0) {
                    char detail[160]{};
                    std::snprintf(detail, sizeof(detail), "tls=%s stack=%d cert_flags=0x%x",
                                  esp_err_to_name(event->error_handle.esp_tls_last_esp_err),
                                  event->error_handle.esp_tls_stack_err,
                                  event->error_handle.esp_tls_cert_verify_flags);
                    chatai::diagnostics::Add("TLS", "FAIL", detail);
                }
                if (event->error_handle.esp_ws_handshake_status_code != 0) {
                    const int http_status = event->error_handle.esp_ws_handshake_status_code;
                    if (http_status == 401 || http_status == 403) {
                        char auth_detail[64]{};
                        std::snprintf(auth_detail, sizeof(auth_detail), "HTTP %d during WSS handshake", http_status);
                        chatai::diagnostics::Add("AUTH", "FAIL", auth_detail);
                    }
                    char detail[80]{};
                    std::snprintf(detail, sizeof(detail), "HTTP status=%d",
                                  event->error_handle.esp_ws_handshake_status_code);
                    chatai::diagnostics::Add("WS_HANDSHAKE", "FAIL", detail);
                }
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
            g_waiting_setup.store(false);
            g_ready.store(false);
            g_speaking.store(false);
            g_error.store(true);
            g_audio_epoch.fetch_add(1);
            ClearAudioQueue();
            ESP_LOGW(kTag, "Gemini Live WebSocket disconnected");
            chatai::diagnostics::Add("WS", "FAIL", "disconnected");
            chatai::display::Notify("GEMINI", "Mat ket noi WSS", 4000);
            break;

        case WEBSOCKET_EVENT_CLOSED:
            g_waiting_setup.store(false);
            g_ready.store(false);
            g_speaking.store(false);
            g_error.store(true);
            g_audio_epoch.fetch_add(1);
            ClearAudioQueue();
            ESP_LOGW(kTag, "Gemini Live WebSocket closed; code=%d",
                     event != nullptr ? event->close_status_code : 0);
            { char detail[64]{}; std::snprintf(detail, sizeof(detail), "close_code=%d",
                event != nullptr ? event->close_status_code : 0);
              chatai::diagnostics::Add("WS", "FAIL", detail); }
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
    bool audio_stream_active = false;

    while (true) {
        bool transport_ready = g_ready.load() &&
            chatai::network::NetworkManager::IsInternetAvailable();
        {
            std::lock_guard<std::mutex> lock(g_send_mutex);
            transport_ready = transport_ready && g_client != nullptr &&
                esp_websocket_client_is_connected(g_client);
        }
        if (!transport_ready || !ShouldListen()) {
            if (audio_stream_active && transport_ready) {
                static constexpr char kAudioStreamEnd[] =
                    "{\"realtimeInput\":{\"audioStreamEnd\":true}}";
                std::lock_guard<std::mutex> lock(g_send_mutex);
                if (g_client != nullptr && esp_websocket_client_is_connected(g_client)) {
                    esp_websocket_client_send_text(g_client, kAudioStreamEnd,
                        sizeof(kAudioStreamEnd) - 1, pdMS_TO_TICKS(1000));
                }
            }
            audio_stream_active = false;
            if (microphone_enabled) {
                chatai::audio::EnableMicrophone(false);
                microphone_enabled = false;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
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
            chatai::diagnostics::Add("AUDIO_IN", "OK", "PCM16 mono 16000 Hz streaming");
        }

        size_t samples_read = 0;
        const esp_err_t read_result = chatai::audio::ReadMicrophonePcm16(
            pcm, kMicChunkSamples, &samples_read, 100);
        if (read_result == ESP_ERR_TIMEOUT) continue;
        if (read_result != ESP_OK || samples_read == 0) {
            ESP_LOGW(kTag, "Microphone read failed: %s", esp_err_to_name(read_result));
            chatai::audio::EnableMicrophone(false);
            microphone_enabled = false;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        size_t encoded_size = 0;
        const int encode_result = mbedtls_base64_encode(
            encoded, sizeof(encoded), &encoded_size,
            reinterpret_cast<const unsigned char*>(pcm), samples_read * sizeof(int16_t));
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
            } else {
                audio_stream_active = true;
                if (!g_audio_input_reported.exchange(true))
                    chatai::diagnostics::Add("AUDIO_IN", "OK", "microphone PCM sent to Gemini Live");
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
                g_speaking.store(false);
            }
            continue;
        }

        if (chunk == nullptr) continue;
        if (chunk->epoch != g_audio_epoch.load() || !g_ready.load()) {
            std::free(chunk);
            chunk = nullptr;
            continue;
        }

        if (!speaker_enabled) {
            const esp_err_t enable_result = chatai::audio::EnableSpeaker(true);
            if (enable_result != ESP_OK) {
                g_error.store(true);
                ESP_LOGE(kTag, "Could not enable speaker: %s", esp_err_to_name(enable_result));
                std::free(chunk);
                chunk = nullptr;
                continue;
            }
            speaker_enabled = true;
            if (!g_speaker_started_reported.exchange(true))
                chatai::diagnostics::Add("SPEAKER", "OK", "I2S playback started");
        }

        g_speaking.store(true);
        size_t samples_written = 0;
        const esp_err_t write_result = chatai::audio::WriteSpeakerPcm16(
            chunk->samples, chunk->sample_count, &samples_written, 100);
        if (write_result != ESP_OK || samples_written != chunk->sample_count) {
            ESP_LOGW(kTag, "Speaker write incomplete: %s", esp_err_to_name(write_result));
        }
        std::free(chunk);
        chunk = nullptr;
    }
}

void GeminiWorker(void*) {
    while (true) {
        bool has_client = false;
        {
            std::lock_guard<std::mutex> lock(g_send_mutex);
            has_client = g_client != nullptr;
        }
        if (has_client) {
            if (g_restart_requested.exchange(false)) {
                esp_websocket_client_handle_t old_client = nullptr;
                {
                    std::lock_guard<std::mutex> lock(g_send_mutex);
                    old_client = g_client;
                    g_client = nullptr;
                }
                const esp_err_t stop_result = old_client != nullptr
                    ? esp_websocket_client_stop(old_client) : ESP_OK;
                if (old_client != nullptr) esp_websocket_client_destroy(old_client);
                g_ready.store(false);
                g_waiting_setup.store(false);
                g_speaking.store(false);
                g_error.store(false);
                g_audio_epoch.fetch_add(1);
                ClearAudioQueue();
                ESP_LOGI(kTag, "Gemini Live client restarted to apply updated credentials");
                if (stop_result != ESP_OK) {
                    ESP_LOGW(kTag, "Gemini client stop returned: %s", esp_err_to_name(stop_result));
                }
                continue;
            }
            if (g_waiting_setup.load()) {
                const uint32_t elapsed = static_cast<uint32_t>(xTaskGetTickCount()) -
                                          g_setup_started_tick.load();
                if (elapsed > pdMS_TO_TICKS(20000) &&
                    g_waiting_setup.exchange(false)) {
                    g_ready.store(false);
                    g_error.store(true);
                    ESP_LOGE(kTag, "Gemini Live did not send setupComplete within 20 seconds");
                    chatai::diagnostics::Add("SETUP", "FAIL", "setupComplete timeout after 20 seconds");
                    chatai::display::Notify("GEMINI", "Qua han setup", 4000);
                    esp_websocket_client_handle_t client = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(g_send_mutex);
                        client = g_client;
                    }
                    if (client != nullptr) {
                        esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (g_restart_requested.exchange(false)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!chatai::network::NetworkManager::IsInternetAvailable()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        std::string api_key = chatai::settings::GetGeminiApiKey();
        if (api_key.empty()) {
            if (!g_missing_key_reported) {
                ESP_LOGW(kTag, "Gemini API Key is not configured; waiting for Web Admin");
                chatai::diagnostics::Add("AUTH", "FAIL", "API Key missing in NVS");
                g_missing_key_reported = true;
            }
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
            continue;
        }

        g_missing_key_reported = false;
        if (!EnsureSystemTime()) {
            std::fill(api_key.begin(), api_key.end(), '\0');
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
            continue;
        }

        if (!g_auth_reported.exchange(true))
            chatai::diagnostics::Add("AUTH", "INFO", "API Key present in NVS; credential value redacted");
        if (!g_tls_attempt_reported.exchange(true))
            chatai::diagnostics::Add("TLS", "INFO", "starting verified TLS/WSS connection");
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
            g_error.store(true);
            ESP_LOGE(kTag, "Could not initialize Gemini Live WebSocket client");
            chatai::diagnostics::Add("WS", "FAIL", "esp_websocket_client_init returned null");
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_send_mutex);
            g_client = client;
        }
        esp_err_t result = esp_websocket_register_events(
            client, WEBSOCKET_EVENT_ANY, WebSocketEventHandler, nullptr);
        if (result == ESP_OK) result = esp_websocket_client_start(client);

        if (result != ESP_OK) {
            g_error.store(true);
            ESP_LOGE(kTag, "Could not start Gemini Live WebSocket: %s",
                     esp_err_to_name(result));
            chatai::diagnostics::Add("WS", "FAIL", esp_err_to_name(result));
            {
                std::lock_guard<std::mutex> lock(g_send_mutex);
                if (g_client == client) g_client = nullptr;
            }
            esp_websocket_client_destroy(client);
            vTaskDelay(pdMS_TO_TICKS(kRetryDelayMs));
            continue;
        }

        ESP_LOGI(kTag, "Gemini Live connection started; waiting for setupComplete");
        chatai::diagnostics::Add("MODEL", "INFO", "models/gemini-3.8-live; response modality AUDIO");
        chatai::diagnostics::Add("WS", "INFO", "client start accepted; waiting for handshake");
        chatai::diagnostics::Add("WS", "INFO", "client start accepted; waiting for handshake");
    }
}

}  // namespace

namespace chatai::gemini {

esp_err_t Initialize() {
    bool expected = false;
    if (!g_task_started.compare_exchange_strong(expected, true)) return ESP_OK;

    g_audio_queue = xQueueCreate(kAudioQueueLength, sizeof(AudioChunk*));
    if (g_audio_queue == nullptr) {
        g_task_started.store(false);
        ESP_LOGE(kTag, "Could not create Gemini playback queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t result = xTaskCreate(
        SpeakerWorker, "chatai_spk", 4096, nullptr, 5, nullptr);
    if (result == pdPASS) {
        result = xTaskCreate(MicrophoneWorker, "chatai_mic", 6144, nullptr, 5, nullptr);
    }
    if (result == pdPASS) {
        result = xTaskCreate(GeminiWorker, "chatai_gemini", 6144, nullptr, 5, nullptr);
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

bool IsSpeaking() {
    return g_speaking.load();
}

bool HasError() {
    return g_error.load();
}

void SetChatEnabled(bool enabled) { g_chat_enabled.store(enabled); }
bool IsChatEnabled() { return g_chat_enabled.load(); }
void SetPushToTalk(bool pressed) { g_push_to_talk.store(pressed); }
bool IsListening() { return ShouldListen(); }

void RequestReconnect() {
    g_restart_requested.store(true);
    g_ready.store(false);
    g_audio_epoch.fetch_add(1);
    ClearAudioQueue();
}

}  // namespace chatai::gemini
