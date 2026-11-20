#include "gemini_live_client.h"

#include <algorithm>
#include <atomic>
#include <cmath>
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
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_websocket_client.h>
#include <esp_tls.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <mbedtls/error.h>
#include <mbedtls/esp_debug.h>
#include <mbedtls/version.h>

#include "audio_manager.h"
#include "gemini_settings.h"
#include "network_manager.h"
#include "diagnostic_manager.h"
#include "display_manager.h"

namespace {
constexpr char kTag[] = "ChatAI-Gemini";
constexpr char kGeminiHost[] = "generativelanguage.googleapis.com";
constexpr char kEndpoint[] =
    "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta."
    "GenerativeService.BidiGenerateContent?key=";
constexpr size_t kMaxIncomingMessage = 8192;
constexpr size_t kMaxOutputSamples = 2048;
constexpr size_t kMicChunkSamples = 1600;  // 100 ms at 16 kHz.
constexpr size_t kMicEncodedBufferSize = 4300;
constexpr size_t kMicMessageBufferSize = 5000;
constexpr uint32_t kFailureSummaryIntervalMs = 300000;
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
bool g_sntp_started = false;
bool g_sntp_sync_confirmed = false;
std::atomic<uint32_t> g_audio_epoch{0};
std::atomic<bool> g_discard_current_response{false};
std::atomic<bool> g_stop_speaker_requested{false};
std::atomic<bool> g_audio_response_reported{false};
std::atomic<bool> g_audio_reject_reported{false};
std::atomic<bool> g_server_content_reported{false};
std::atomic<int> g_last_speech_state{-1};
std::atomic<bool> g_audio_input_reported{false};
std::atomic<uint32_t> g_ws_session_start_tick{0};
std::atomic<uint32_t> g_ws_ping_rx_count{0};
std::atomic<uint32_t> g_ws_pong_rx_count{0};
std::atomic<uint32_t> g_ws_last_pong_tick{0};
std::atomic<uint32_t> g_ws_app_tx_count{0};
std::atomic<uint32_t> g_ws_last_app_tx_tick{0};
std::atomic<uint32_t> g_ws_audio_tx_count{0};
std::atomic<uint32_t> g_ws_last_audio_tx_tick{0};
std::atomic<uint32_t> g_ws_stream_end_count{0};
std::atomic<uint32_t> g_ws_last_stream_end_tick{0};
std::atomic<uint32_t> g_ws_server_content_count{0};
std::atomic<uint32_t> g_ws_last_server_content_tick{0};
std::atomic<uint32_t> g_ws_audio_rx_count{0};
std::atomic<uint32_t> g_ws_last_audio_rx_tick{0};
std::atomic<bool> g_ws_summary_reported{false};
std::atomic<bool> g_speaker_started_reported{false};
std::atomic<bool> g_auth_reported{false};
std::atomic<bool> g_tls_attempt_reported{false};
std::atomic<bool> g_model_reported{false};
std::atomic<bool> g_ws_start_reported{false};
std::atomic<bool> g_tls_bundle_success_reported{false};
std::atomic<bool> g_tls_bundle_failure_reported{false};
std::atomic<bool> g_tls_config_reported{false};
bool g_tls_probe_completed = false;
std::atomic<bool> g_client_failed{false};
std::atomic<bool> g_connection_failure_reported{false};
std::atomic<bool> g_connection_notification_active{false};
std::atomic<bool> g_time_failure_reported{false};
std::atomic<bool> g_time_success_reported{false};
std::atomic<bool> g_time_initial_sample_reported{false};
std::atomic<bool> g_tls_fatal{false};
std::atomic<bool> g_retry_blocked{false};
std::atomic<bool> g_tls_failure_active{false};
std::atomic<int> g_last_tls_error{0};
std::atomic<uint32_t> g_last_tls_cert_flags{0};
std::atomic<uint32_t> g_tls_suppressed_count{0};
std::atomic<uint32_t> g_tls_failure_started_tick{0};
std::atomic<uint32_t> g_tls_last_summary_tick{0};
std::atomic<uint32_t> g_client_generation{0};
std::mutex g_event_mutex;
esp_websocket_client_handle_t g_client = nullptr;
QueueHandle_t g_audio_queue = nullptr;
enum class IncomingMessageType : uint8_t { kNone, kText, kBinary };
std::string g_incoming_message;
size_t g_incoming_frame_bytes = 0;
IncomingMessageType g_incoming_type = IncomingMessageType::kNone;
bool g_missing_key_reported = false;
std::mutex g_send_mutex;

bool ShouldListen() {
    return g_chat_enabled.load() || g_push_to_talk.load();
}

uint32_t ElapsedSeconds(uint32_t now, uint32_t then) {
    return (now - then) / configTICK_RATE_HZ;
}

void ResetWebSocketSessionMetrics() {
    g_ws_session_start_tick.store(static_cast<uint32_t>(xTaskGetTickCount()));
    g_ws_ping_rx_count.store(0);
    g_ws_pong_rx_count.store(0);
    g_ws_last_pong_tick.store(0);
    g_ws_app_tx_count.store(0);
    g_ws_last_app_tx_tick.store(0);
    g_ws_audio_tx_count.store(0);
    g_ws_last_audio_tx_tick.store(0);
    g_ws_stream_end_count.store(0);
    g_ws_last_stream_end_tick.store(0);
    g_ws_server_content_count.store(0);
    g_ws_last_server_content_tick.store(0);
    g_ws_audio_rx_count.store(0);
    g_ws_last_audio_rx_tick.store(0);
    g_audio_response_reported.store(false);
    g_audio_reject_reported.store(false);
    g_server_content_reported.store(false);
    g_last_speech_state.store(-1);
    g_ws_summary_reported.store(false);
}

void ReportWebSocketSessionSummary(const char* source) {
    if (g_ws_summary_reported.exchange(true)) return;

    const uint32_t now = static_cast<uint32_t>(xTaskGetTickCount());
    const uint32_t started = g_ws_session_start_tick.load();
    const uint32_t pong_count = g_ws_pong_rx_count.load();
    const uint32_t app_tx_count = g_ws_app_tx_count.load();
    const uint32_t audio_tx_count = g_ws_audio_tx_count.load();
    const uint32_t stream_end_count = g_ws_stream_end_count.load();
    const uint32_t server_content_count = g_ws_server_content_count.load();
    const uint32_t audio_rx_count = g_ws_audio_rx_count.load();
    const int pong_age = pong_count > 0
        ? static_cast<int>(ElapsedSeconds(now, g_ws_last_pong_tick.load())) : -1;
    const int app_tx_age = app_tx_count > 0
        ? static_cast<int>(ElapsedSeconds(now, g_ws_last_app_tx_tick.load())) : -1;
    const int audio_tx_age = audio_tx_count > 0
        ? static_cast<int>(ElapsedSeconds(now, g_ws_last_audio_tx_tick.load())) : -1;
    const int stream_end_age = stream_end_count > 0
        ? static_cast<int>(ElapsedSeconds(now, g_ws_last_stream_end_tick.load())) : -1;
    const int server_content_age = server_content_count > 0
        ? static_cast<int>(ElapsedSeconds(now, g_ws_last_server_content_tick.load())) : -1;
    const int audio_rx_age = audio_rx_count > 0
        ? static_cast<int>(ElapsedSeconds(now, g_ws_last_audio_rx_tick.load())) : -1;

    char detail[384]{};
    std::snprintf(detail, sizeof(detail),
                  "source=%s session_s=%lu ping_rx=%lu pong_rx=%lu pong_age_s=%d "
                  "client_ping_tx=library_internal_not_exposed ping_interval_s=20 "
                  "app_tx=%lu app_tx_age_s=%d audio_tx=%lu audio_tx_age_s=%d "
                  "audioStreamEnd=%lu stream_end_age_s=%d server_content=%lu "
                  "server_content_age_s=%d audio_rx=%lu audio_rx_age_s=%d",
                  source != nullptr ? source : "unknown",
                  static_cast<unsigned long>(ElapsedSeconds(now, started)),
                  static_cast<unsigned long>(g_ws_ping_rx_count.load()),
                  static_cast<unsigned long>(pong_count), pong_age,
                  static_cast<unsigned long>(app_tx_count), app_tx_age,
                  static_cast<unsigned long>(audio_tx_count), audio_tx_age,
                  static_cast<unsigned long>(stream_end_count), stream_end_age,
                  static_cast<unsigned long>(server_content_count), server_content_age,
                  static_cast<unsigned long>(audio_rx_count), audio_rx_age);
    chatai::diagnostics::Add("WS_SESSION", "INFO", detail);
}

void ReportWebSocketCloseFrame(const esp_websocket_event_data_t* event) {
    if (event == nullptr) return;

    char reason[97]{};
    int reason_length = 0;
    // RFC 6455 CLOSE payload starts with a two-byte status code, followed by
    // an optional UTF-8 reason. Keep only bounded printable ASCII in diagnostics.
    if (event->data_ptr != nullptr && event->payload_offset == 0 &&
        event->data_len >= 2 && event->payload_len >= 2) {
        const int available = std::min(event->data_len, event->payload_len) - 2;
        const int copy_length = std::min(available, static_cast<int>(sizeof(reason) - 1));
        for (int i = 0; i < copy_length; ++i) {
            const unsigned char value =
                static_cast<unsigned char>(event->data_ptr[i + 2]);
            reason[i] = value >= 0x20 && value <= 0x7e
                ? static_cast<char>(value) : '?';
            ++reason_length;
        }
    }

    char detail[192]{};
    std::snprintf(detail, sizeof(detail),
                  "source=received_close_frame close_code=%d payload_len=%d "
                  "reason_len_logged=%d reason=\"%s\"",
                  event->close_status_code, event->payload_len,
                  reason_length, reason_length > 0 ? reason : "empty_or_unavailable");
    chatai::diagnostics::Add("WS_CLOSE", "INFO", detail);
}

void ReportTlsFailure(const char* detail, int error_code, uint32_t cert_flags) {
    const uint32_t now = static_cast<uint32_t>(xTaskGetTickCount());
    const bool was_active = g_tls_failure_active.exchange(true);
    const int previous_error = g_last_tls_error.exchange(error_code);
    const uint32_t previous_flags = g_last_tls_cert_flags.exchange(cert_flags);

    if (!was_active || previous_error != error_code || previous_flags != cert_flags) {
        g_tls_suppressed_count.store(0);
        g_tls_failure_started_tick.store(now);
        g_tls_last_summary_tick.store(now);
        chatai::diagnostics::Add("TLS", "FAIL", detail);
        return;
    }

    const uint32_t suppressed = g_tls_suppressed_count.fetch_add(1) + 1;
    const uint32_t last_summary = g_tls_last_summary_tick.load();
    if (now - last_summary >= pdMS_TO_TICKS(kFailureSummaryIntervalMs)) {
        char summary[128]{};
        const uint32_t duration = (now - g_tls_failure_started_tick.load()) /
                                  configTICK_RATE_HZ;
        std::snprintf(summary, sizeof(summary),
                      "same_error_count=%lu duration=%lus",
                      static_cast<unsigned long>(suppressed),
                      static_cast<unsigned long>(duration));
        chatai::diagnostics::Add("TLS", "WARN", summary);
        g_tls_last_summary_tick.store(now);
    }
}

void ReportTlsRecovered() {
    if (!g_tls_failure_active.exchange(false)) return;
    const uint32_t now = static_cast<uint32_t>(xTaskGetTickCount());
    const uint32_t failures = g_tls_suppressed_count.exchange(0) + 1;
    const uint32_t duration = (now - g_tls_failure_started_tick.load()) /
                              configTICK_RATE_HZ;
    char detail[128]{};
    std::snprintf(detail, sizeof(detail),
                  "recovered_after_failures=%lu duration=%lus",
                  static_cast<unsigned long>(failures),
                  static_cast<unsigned long>(duration));
    chatai::diagnostics::Add("TLS", "OK", detail);
    g_tls_fatal.store(false);
    g_retry_blocked.store(false);
}

void NotifyConnectionError(const char* title, const char* message) {
    if (!g_connection_notification_active.exchange(true)) {
        chatai::display::Notify(title, message, 4000);
    }
}

void ReportTimeFailure(const char* detail) {
    chatai::diagnostics::SetTimeSyncStatus(false);
    if (!g_time_failure_reported.exchange(true)) {
        ESP_LOGW(kTag, "System time is not NTP-verified for Gemini TLS: %s", detail);
        chatai::diagnostics::Add("TIME", "FAIL", detail);
    }
}

void ReportTimeSuccess(const char* detail) {
    chatai::diagnostics::SetTimeSyncStatus(true, "SNTP");
    const bool recovered = g_time_failure_reported.exchange(false);
    if (recovered || !g_time_success_reported.exchange(true)) {
        char time_detail[128]{};
        std::snprintf(time_detail, sizeof(time_detail), "%s; timezone=Asia/Ho_Chi_Minh UTC+07:00",
                      recovered ? "system clock synchronized" : detail);
        chatai::diagnostics::Add("TIME", "OK", time_detail);
    }
}

esp_err_t AttachGeminiCertificateBundle(void* tls_config) {
    const esp_err_t result = esp_crt_bundle_attach(tls_config);
#if defined(CONFIG_MBEDTLS_DEBUG) && CONFIG_MBEDTLS_DEBUG
    if (result == ESP_OK && tls_config != nullptr) {
        mbedtls_esp_enable_debug_log(
            static_cast<mbedtls_ssl_config*>(tls_config), CONFIG_MBEDTLS_DEBUG_LEVEL);
        esp_log_level_set("esp-tls", ESP_LOG_DEBUG);
        esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_DEBUG);
        char debug_detail[80]{};
        std::snprintf(debug_detail, sizeof(debug_detail),
                      "MbedTLS debug compiled in; level=%d", CONFIG_MBEDTLS_DEBUG_LEVEL);
        chatai::diagnostics::Add("TLS_DEBUG", "OK", debug_detail);
    }
#else
    chatai::diagnostics::Add("TLS_DEBUG", "FAIL",
                             "CONFIG_MBEDTLS_DEBUG is disabled in this firmware build");
#endif
    char detail[112]{};
    std::snprintf(detail, sizeof(detail), "esp_crt_bundle_attach=%s(0x%08x)",
                  esp_err_to_name(result), static_cast<unsigned>(result));
    if (result == ESP_OK) {
        if (!g_tls_bundle_success_reported.exchange(true)) {
            ESP_LOGI(kTag, "Gemini certificate bundle attached");
            chatai::diagnostics::Add("TLS_CA", "OK", detail);
        }
    } else if (!g_tls_bundle_failure_reported.exchange(true)) {
        ESP_LOGE(kTag, "Gemini certificate bundle attach failed: %s",
                 esp_err_to_name(result));
        chatai::diagnostics::Add("TLS_CA", "FAIL", detail);
    }
    if (result == ESP_OK && !g_tls_config_reported.exchange(true)) {
        const char* cross_signed = "disabled";
        const char* time_date = "disabled";
        const char* full_bundle = "disabled";
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY
        cross_signed = "enabled";
#endif
#if defined(CONFIG_MBEDTLS_HAVE_TIME_DATE) && CONFIG_MBEDTLS_HAVE_TIME_DATE
        time_date = "enabled";
#endif
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL
        full_bundle = "enabled";
#endif
        char config_detail[192]{};
        std::snprintf(config_detail, sizeof(config_detail),
                      "mbedtls=%s cross_signed_verify=%s time_date=%s full_bundle=%s debug_level=%d",
                      MBEDTLS_VERSION_STRING_FULL, cross_signed, time_date,
                      full_bundle, CONFIG_MBEDTLS_DEBUG_LEVEL);
        chatai::diagnostics::Add("TLS_CONFIG", "INFO", config_detail);
    }
    return result;
}

bool ProbeGeminiTls() {
    esp_tls_cfg_t config{};
    config.crt_bundle_attach = AttachGeminiCertificateBundle;
    config.timeout_ms = 12000;

    esp_tls_t* tls = esp_tls_init();
    if (tls == nullptr) {
        chatai::diagnostics::Add("TLS_PROBE", "FAIL", "esp_tls_init returned null");
        return false;
    }

    const int connected = esp_tls_conn_new_sync(
        kGeminiHost, sizeof(kGeminiHost) - 1, 443, &config, tls);
    if (connected == 1) {
        chatai::diagnostics::Add(
            "TLS_PROBE", "OK",
            "independent verified TLS handshake succeeded; hostname and CA bundle accepted");
        esp_tls_conn_destroy(tls);
        return true;
    }

    esp_tls_error_handle_t error_handle = nullptr;
    int mbedtls_error = 0;
    int cert_flags = 0;
    esp_err_t tls_error = ESP_FAIL;
    if (esp_tls_get_error_handle(tls, &error_handle) == ESP_OK &&
        error_handle != nullptr) {
        tls_error = esp_tls_get_and_clear_last_error(
            error_handle, &mbedtls_error, &cert_flags);
    }

    // esp_tls_get_and_clear_last_error() stores the MbedTLS stack code
    // as a positive value; mbedtls_strerror() expects the original negative code.
    const int normalized_mbedtls_error =
        mbedtls_error > 0 ? -mbedtls_error : mbedtls_error;
    char mbedtls_message[112]{};
    if (normalized_mbedtls_error != 0) {
        mbedtls_strerror(normalized_mbedtls_error, mbedtls_message,
                         sizeof(mbedtls_message));
    } else {
        std::snprintf(mbedtls_message, sizeof(mbedtls_message), "not reported");
    }

    const size_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const std::time_t system_time = std::time(nullptr);

    char detail[416]{};
    std::snprintf(detail, sizeof(detail),
                  "host=%s result=%d esp=%s(0x%08x) mbedtls_raw=%d normalized=%d "
                  "text=%s cert_flags=0x%x internal_free=%lu internal_largest=%lu "
                  "unix_time=%lld",
                  kGeminiHost, connected, esp_err_to_name(tls_error),
                  static_cast<unsigned>(tls_error), mbedtls_error,
                  normalized_mbedtls_error, mbedtls_message,
                  static_cast<unsigned>(cert_flags),
                  static_cast<unsigned long>(internal_free),
                  static_cast<unsigned long>(internal_largest),
                  static_cast<long long>(system_time));
    ESP_LOGE(kTag, "Independent Gemini TLS probe failed: %s", detail);
    chatai::diagnostics::Add("TLS_PROBE", "FAIL", detail);
    esp_tls_conn_destroy(tls);
    return false;
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
    constexpr unsigned kSntpServerCount = 3;
    if (!g_time_initial_sample_reported.exchange(true)) {
        char detail[96]{};
        std::snprintf(detail, sizeof(detail), "pre-SNTP clock sample unix_time=%lld",
                      static_cast<long long>(std::time(nullptr)));
        chatai::diagnostics::Add("TIME", "INFO", detail);
    }
    // A plausible Unix epoch is not proof that the clock is correct. Require
    // one successful SNTP synchronization after this boot before TLS starts.
    if (g_sntp_sync_confirmed && std::time(nullptr) >= kMinimumValidTime) {
        ReportTimeSuccess("SNTP clock remains valid");
        return true;
    }

    if (!g_sntp_initialized) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            kSntpServerCount,
            ESP_SNTP_SERVER_LIST("time.google.com", "pool.ntp.org",
                                 "time.cloudflare.com"));
        config.start = false;
        const esp_err_t result = esp_netif_sntp_init(&config);
        if (result != ESP_OK) {
            ReportTimeFailure(esp_err_to_name(result));
            return false;
        }
        g_sntp_initialized = true;
    }

    // Start once. Calling start again restarts the SNTP service and can
    // postpone the next request while GeminiWorker is retrying.
    if (!g_sntp_started) {
        const esp_err_t start_result = esp_netif_sntp_start();
        if (start_result != ESP_OK) {
            ReportTimeFailure(esp_err_to_name(start_result));
            return false;
        }
        g_sntp_started = true;
    }

    esp_err_t sync_result = ESP_ERR_TIMEOUT;
    for (unsigned attempt = 0; attempt < 15; ++attempt) {
        sync_result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
        if (sync_result == ESP_OK && std::time(nullptr) >= kMinimumValidTime) {
            g_sntp_sync_confirmed = true;
            ESP_LOGI(kTag, "SNTP synchronization confirmed; Gemini TLS certificate validation can proceed");
            ReportTimeSuccess("SNTP synchronized; configured_servers=time.google.com,pool.ntp.org,time.cloudflare.com");
            return true;
        }
        if (sync_result != ESP_ERR_TIMEOUT &&
            sync_result != ESP_ERR_NOT_FINISHED && sync_result != ESP_OK) {
            break;
        }
    }

    unsigned int reachability[kSntpServerCount]{};
    for (unsigned index = 0; index < kSntpServerCount; ++index) {
        esp_netif_sntp_reachability(index, &reachability[index]);
    }
    char detail[128]{};
    std::snprintf(detail, sizeof(detail),
                  "SNTP not confirmed; sync=%s epoch_valid=%s NTP_reachability=%02x/%02x/%02x",
                  esp_err_to_name(sync_result),
                  std::time(nullptr) >= kMinimumValidTime ? "yes_unverified" : "no",
                  reachability[0], reachability[1], reachability[2]);
    ReportTimeFailure(detail);
    return false;
}

std::string BuildSetupMessage() {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return {};
    cJSON* setup = cJSON_AddObjectToObject(root, "setup");
    cJSON_AddStringToObject(setup, "model", "models/gemini-3.8-live");
    cJSON* generation = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON* modalities = cJSON_AddArrayToObject(generation, "responseModalities");
    cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO"));
    cJSON* speech = cJSON_AddObjectToObject(generation, "speechConfig");
    cJSON* voice_config = cJSON_AddObjectToObject(speech, "voiceConfig");
    cJSON* prebuilt = cJSON_AddObjectToObject(voice_config, "prebuiltVoiceConfig");
    const std::string voice = chatai::settings::GetGeminiVoice();
    cJSON_AddStringToObject(prebuilt, "voiceName", voice.c_str());
    cJSON* instruction = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON* parts = cJSON_AddArrayToObject(instruction, "parts");
    const std::string prompt = chatai::settings::GetGeminiSystemPrompt();
    cJSON* part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", prompt.c_str());
    cJSON_AddItemToArray(parts, part);
    char* serialized = cJSON_PrintUnformatted(root);
    std::string message = serialized != nullptr ? serialized : "";
    if (serialized != nullptr) cJSON_free(serialized);
    cJSON_Delete(root);
    return message;
}

void ClearAudioQueue() {
    if (g_audio_queue == nullptr) return;
    AudioChunk* chunk = nullptr;
    while (xQueueReceive(g_audio_queue, &chunk, 0) == pdTRUE) {
        std::free(chunk);
    }
}

void ReportAudioRejectOnce(const char* reason) {
    if (!g_audio_reject_reported.exchange(true)) {
        chatai::diagnostics::Add("GEMINI_AUDIO_RX", "WARN", reason);
    }
}

void QueueServerAudio(const cJSON* inline_data) {
    if (g_discard_current_response.load()) return;
    const cJSON* mime = cJSON_GetObjectItemCaseSensitive(inline_data, "mimeType");
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(inline_data, "data");
    if (!cJSON_IsString(mime) || !cJSON_IsString(data) ||
        data->valuestring == nullptr || g_audio_queue == nullptr) {
        ReportAudioRejectOnce("audio inlineData missing MIME/data or playback queue unavailable");
        return;
    }
    if (std::strncmp(mime->valuestring, "audio/pcm;rate=24000", 20) != 0) {
        char detail[112]{};
        std::snprintf(detail, sizeof(detail), "unsupported audio MIME type: %.80s",
                      mime->valuestring);
        ReportAudioRejectOnce(detail);
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
    if (g_discard_current_response.load() || chunk->epoch != g_audio_epoch.load()) {
        std::free(chunk);
        return;
    }
    if (xQueueSend(g_audio_queue, &chunk, 0) != pdTRUE) {
        std::free(chunk);
        ESP_LOGW(kTag, "Gemini playback queue full; dropping audio chunk");
    } else {
        g_ws_audio_rx_count.fetch_add(1);
        g_ws_last_audio_rx_tick.store(static_cast<uint32_t>(xTaskGetTickCount()));
        if (!g_audio_response_reported.exchange(true)) {
            int16_t pcm_min = INT16_MAX;
            int16_t pcm_max = INT16_MIN;
            for (size_t i = 0; i < chunk->sample_count; ++i) {
                pcm_min = std::min(pcm_min, chunk->samples[i]);
                pcm_max = std::max(pcm_max, chunk->samples[i]);
            }
            char detail[192]{};
            std::snprintf(detail, sizeof(detail),
                          "mime=%s encoded_bytes=%lu decoded_bytes=%lu samples=%lu "
                          "pcm_min=%d pcm_max=%d queue_depth=%lu",
                          mime->valuestring,
                          static_cast<unsigned long>(encoded_size),
                          static_cast<unsigned long>(decoded_size),
                          static_cast<unsigned long>(chunk->sample_count),
                          static_cast<int>(pcm_min), static_cast<int>(pcm_max),
                          static_cast<unsigned long>(uxQueueMessagesWaiting(g_audio_queue)));
            chatai::diagnostics::SetCrashContext("GEMINI_AUDIO_RX", detail);
            chatai::diagnostics::Add("GEMINI_AUDIO_RX", "INFO", detail);
        }
        g_speaking.store(true);
    }
}

void HandleServerMessage(const std::string& message) {
    cJSON* root = cJSON_ParseWithLength(message.c_str(), message.size());
    if (root == nullptr) {
        ESP_LOGW(kTag, "Gemini sent an invalid JSON message");
        chatai::diagnostics::Add("GEMINI_PROTOCOL", "FAIL", "invalid JSON response");
        return;
    }

    const cJSON* setup_complete = cJSON_GetObjectItemCaseSensitive(root, "setupComplete");
    if (cJSON_IsObject(setup_complete)) {
        g_waiting_setup.store(false);
        g_ready.store(true);
        g_error.store(false);
        g_tls_fatal.store(false);
        g_retry_blocked.store(false);
        ReportTlsRecovered();
        g_connection_notification_active.store(false);
        ESP_LOGI(kTag, "Gemini Live setupComplete; audio session is ready");
        chatai::diagnostics::Add("SETUP", "OK", "setupComplete received; Gemini READY");
    }

    const cJSON* server_content =
        cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    if (cJSON_IsObject(server_content)) {
        g_ws_server_content_count.fetch_add(1);
        g_ws_last_server_content_tick.store(static_cast<uint32_t>(xTaskGetTickCount()));

        const cJSON* speech_state =
            cJSON_GetObjectItemCaseSensitive(server_content, "speechState");
        if (cJSON_IsString(speech_state)) {
            const bool speech = std::strcmp(speech_state->valuestring, "SPEECH") == 0;
            const bool non_speech = std::strcmp(speech_state->valuestring, "NON_SPEECH") == 0;
            const int state = speech ? 1 : (non_speech ? 0 : -1);
            if (state >= 0 && g_last_speech_state.exchange(state) != state) {
                chatai::diagnostics::Add("GEMINI_VAD",
                    speech ? "SPEECH" : "NON_SPEECH",
                    speech ? "Gemini server detected incoming speech"
                           : "Gemini server detected end of speech");
            }
        }

        const cJSON* turn_complete =
            cJSON_GetObjectItemCaseSensitive(server_content, "turnComplete");
        if (cJSON_IsTrue(turn_complete)) {
            const cJSON* interaction_status =
                cJSON_GetObjectItemCaseSensitive(server_content, "interactionStatus");
            const cJSON* complete_reason =
                cJSON_GetObjectItemCaseSensitive(server_content, "turnCompleteReason");
            char detail[144]{};
            std::snprintf(detail, sizeof(detail),
                "interaction_status=%.32s reason=%.48s model_turn=%d",
                cJSON_IsString(interaction_status) ? interaction_status->valuestring : "unknown",
                cJSON_IsString(complete_reason) ? complete_reason->valuestring : "unspecified",
                cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(server_content, "modelTurn")) ? 1 : 0);
            chatai::diagnostics::Add("GEMINI_TURN", "COMPLETE", detail);
        } else if (!g_server_content_reported.exchange(true)) {
            const cJSON* model_turn =
                cJSON_GetObjectItemCaseSensitive(server_content, "modelTurn");
            const cJSON* parts = cJSON_IsObject(model_turn)
                ? cJSON_GetObjectItemCaseSensitive(model_turn, "parts") : nullptr;
            char detail[112]{};
            std::snprintf(detail, sizeof(detail),
                "first serverContent received; model_turn=%d parts=%d",
                cJSON_IsObject(model_turn) ? 1 : 0,
                cJSON_IsArray(parts) ? cJSON_GetArraySize(parts) : 0);
            chatai::diagnostics::Add("GEMINI_SERVER_CONTENT", "INFO", detail);
        }

        const cJSON* interrupted =
            cJSON_GetObjectItemCaseSensitive(server_content, "interrupted");
        if (cJSON_IsTrue(interrupted)) {
            g_audio_epoch.fetch_add(1);
            ClearAudioQueue();
            g_discard_current_response.store(false);
            g_speaking.store(false);
            g_stop_speaker_requested.store(true);
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
        if (cJSON_IsTrue(turn_complete)) {
            g_discard_current_response.store(false);
        }
    }

    const cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (error != nullptr) {
        g_waiting_setup.store(false);
        g_ready.store(false);
        g_speaking.store(false);
        g_error.store(true);
        g_client_failed.store(true);
        g_connection_failure_reported.store(true);

        const cJSON* status = cJSON_IsObject(error)
            ? cJSON_GetObjectItemCaseSensitive(error, "status") : nullptr;
        const cJSON* message_text = cJSON_IsObject(error)
            ? cJSON_GetObjectItemCaseSensitive(error, "message") : nullptr;
        if (cJSON_IsString(status) &&
            (std::strcmp(status->valuestring, "UNAUTHENTICATED") == 0 ||
             std::strcmp(status->valuestring, "PERMISSION_DENIED") == 0)) {
            g_retry_blocked.store(true);
        }
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
            NotifyConnectionError("GEMINI API", "Loi phan hoi");
        } else {
            ESP_LOGE(kTag, "Gemini rejected or terminated the Live session");
            chatai::diagnostics::Add("GEMINI_API", "FAIL", "error response without status/message");
        }
    }

    cJSON_Delete(root);
}

void ReportWebSocketDisconnectError(const esp_websocket_event_data_t* event) {
    if (event == nullptr) {
        chatai::diagnostics::Add("WS", "FAIL", "disconnect event had no data");
        return;
    }

    switch (event->error_handle.error_type) {
        case WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT: {
            const auto& error = event->error_handle;
            const int stack_error = error.esp_tls_stack_err;
            const int mbedtls_error = stack_error > 0 ? -stack_error : stack_error;
            char mbedtls_message[112]{};
            if (mbedtls_error != 0) {
                mbedtls_strerror(mbedtls_error, mbedtls_message,
                                 sizeof(mbedtls_message));
            } else {
                std::snprintf(mbedtls_message, sizeof(mbedtls_message),
                              "not reported");
            }
    
            const size_t internal_free =
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            const size_t internal_largest =
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            const std::time_t system_time = std::time(nullptr);
    
            char detail[416]{};
            std::snprintf(detail, sizeof(detail),
                          "host=generativelanguage.googleapis.com esp=%s(0x%08x) "
                          "stack_raw=%d(0x%08x) mbedtls_code=%d text=%s "
                          "cert_flags=0x%x socket_errno=%d unix_time=%lld "
                          "internal_free=%lu internal_largest=%lu",
                          esp_err_to_name(error.esp_tls_last_esp_err),
                          static_cast<unsigned>(error.esp_tls_last_esp_err),
                          stack_error, static_cast<unsigned>(stack_error),
                          mbedtls_error, mbedtls_message,
                          error.esp_tls_cert_verify_flags,
                          error.esp_transport_sock_errno,
                          static_cast<long long>(system_time),
                          static_cast<unsigned long>(internal_free),
                          static_cast<unsigned long>(internal_largest));
            if (mbedtls_error != 0 || error.esp_tls_cert_verify_flags != 0) {
                const bool fatal_tls = mbedtls_error == -0x3000 ||
                                       error.esp_tls_cert_verify_flags != 0;
                if (fatal_tls) {
                    g_tls_fatal.store(true);
                    g_retry_blocked.store(true);
                }
                ReportTlsFailure(detail, mbedtls_error,
                                 error.esp_tls_cert_verify_flags);
            } else {
                char socket_detail[80]{};
                std::snprintf(socket_detail, sizeof(socket_detail),
                              "socket_errno=%d tls=%s",
                              error.esp_transport_sock_errno,
                              esp_err_to_name(error.esp_tls_last_esp_err));
                chatai::diagnostics::Add("WS_SOCKET", "FAIL", socket_detail);
            }
            break;
        }
    
        case WEBSOCKET_ERROR_TYPE_HANDSHAKE: {
            const int http_status = event->error_handle.esp_ws_handshake_status_code;
            char detail[80]{};
            std::snprintf(detail, sizeof(detail), "HTTP status=%d", http_status);
            chatai::diagnostics::Add("WS_HANDSHAKE", "FAIL", detail);
            if (http_status == 401 || http_status == 403) {
                g_retry_blocked.store(true);
                chatai::diagnostics::Add("AUTH", "FAIL",
                                         "HTTP authentication rejected during WSS handshake");
            }
            break;
        }
    
        case WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT:
            chatai::diagnostics::Add("WS", "FAIL", "WebSocket PONG timeout");
            break;
    
        case WEBSOCKET_ERROR_TYPE_SERVER_CLOSE:
            chatai::diagnostics::Add("WS", "FAIL", "server closed WebSocket connection");
            break;
    
        case WEBSOCKET_ERROR_TYPE_NONE:
        default: {
            char detail[64]{};
            std::snprintf(detail, sizeof(detail), "error_type=%d",
                          static_cast<int>(event->error_handle.error_type));
            chatai::diagnostics::Add("WS", "FAIL", detail);
            break;
        }
    }
}

void WebSocketEventHandler(void* handler_arg, esp_event_base_t, int32_t event_id, void* event_data) {
    const uint32_t callback_generation =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handler_arg));
    std::lock_guard<std::mutex> event_lock(g_event_mutex);
    if (callback_generation != g_client_generation.load()) return;

    auto* event = static_cast<esp_websocket_event_data_t*>(event_data);

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED: {
            g_ready.store(false);
            g_waiting_setup.store(true);
            g_setup_started_tick.store(static_cast<uint32_t>(xTaskGetTickCount()));
            g_speaking.store(false);
            g_discard_current_response.store(false);
            g_stop_speaker_requested.store(true);
            g_error.store(false);
            g_client_failed.store(false);
            g_connection_failure_reported.store(false);
            g_incoming_message.clear();
            g_incoming_frame_bytes = 0;
            g_incoming_type = IncomingMessageType::kNone;
            ResetWebSocketSessionMetrics();
            ESP_LOGI(kTag, "Gemini Live WebSocket connected; sending setup");
            chatai::diagnostics::Add("WS_KEEPALIVE", "INFO",
                "client_ping_interval_s=20; outgoing Ping is managed internally by esp_websocket_client");
            chatai::diagnostics::Add("WS", "OK", "WebSocket handshake completed");
            chatai::diagnostics::Add("SETUP", "INFO", "setup request sending");

            std::lock_guard<std::mutex> lock(g_send_mutex);
            const std::string setup_message = BuildSetupMessage();
            const int sent = g_client != nullptr && !setup_message.empty()
                ? esp_websocket_client_send_text(
                    g_client, setup_message.c_str(), setup_message.size(), pdMS_TO_TICKS(5000))
                : -1;
            if (sent == static_cast<int>(setup_message.size())) {
                g_ws_app_tx_count.fetch_add(1);
                g_ws_last_app_tx_tick.store(static_cast<uint32_t>(xTaskGetTickCount()));
                chatai::diagnostics::Add("SETUP", "INFO", "setup message sent; waiting for setupComplete");
            }
            if (sent != static_cast<int>(setup_message.size())) {
                g_error.store(true);
                g_client_failed.store(true);
                g_connection_failure_reported.store(true);
                ESP_LOGE(kTag, "Could not send Gemini Live setup message");
                chatai::diagnostics::Add("SETUP", "FAIL", "WebSocket send setup failed");
                NotifyConnectionError("GEMINI", "Loi gui setup");
            }
            break;
        }

        case WEBSOCKET_EVENT_DATA: {
            if (event == nullptr) {
                if (g_waiting_setup.load()) {
                    chatai::diagnostics::Add("WS_RX", "WARN", "DATA event without metadata");
                }
                break;
            }

            const uint8_t opcode = static_cast<uint8_t>(event->op_code);
            const bool setup_diagnostics = g_waiting_setup.load();
            if (setup_diagnostics) {
                char rx_detail[128]{};
                std::snprintf(rx_detail, sizeof(rx_detail),
                              "opcode=0x%02x bytes=%d payload=%d offset=%d fin=%d",
                              opcode, event->data_len, event->payload_len,
                              event->payload_offset, event->fin ? 1 : 0);
                ESP_LOGI(kTag, "WebSocket RX: %s", rx_detail);
                chatai::diagnostics::Add("WS_RX", "INFO", rx_detail);
            }

            // Control frames can interleave fragmented data. Record their
            // metadata without feeding them into Gemini JSON message assembly.
            if (opcode == 0x8) {
                ReportWebSocketCloseFrame(event);
                break;
            }
            if (opcode == 0x9) {
                g_ws_ping_rx_count.fetch_add(1);
                break;
            }
            if (opcode == 0xA) {
                const uint32_t count = g_ws_pong_rx_count.fetch_add(1);
                g_ws_last_pong_tick.store(static_cast<uint32_t>(xTaskGetTickCount()));
                if (count == 0) {
                    chatai::diagnostics::Add("WS_PONG", "OK",
                        "first PONG received; subsequent PONGs counted for session summary");
                }
                break;
            }

            if (event->data_len <= 0 || event->payload_len <= 0 ||
                event->payload_offset < 0 ||
                event->payload_len > static_cast<int>(kMaxIncomingMessage) ||
                event->payload_offset > event->payload_len ||
                event->data_len > event->payload_len - event->payload_offset ||
                event->data_ptr == nullptr) {
                g_incoming_message.clear();
                g_incoming_frame_bytes = 0;
                g_incoming_type = IncomingMessageType::kNone;
                if (setup_diagnostics) {
                    chatai::diagnostics::Add("WS_RX", "WARN",
                        "invalid DATA payload bounds; message assembly reset");
                }
                break;
            }

            if (event->payload_offset == 0) {
                g_incoming_frame_bytes = 0;
                if (opcode == 0x1 || opcode == 0x2) {
                    if (g_incoming_type != IncomingMessageType::kNone &&
                        setup_diagnostics) {
                        chatai::diagnostics::Add("WS_RX", "WARN",
                            "new data message replaced an incomplete message");
                    }
                    g_incoming_message.clear();
                    g_incoming_type = opcode == 0x1
                        ? IncomingMessageType::kText
                        : IncomingMessageType::kBinary;
                } else if (opcode == 0x0) {
                    if (g_incoming_type == IncomingMessageType::kNone) {
                        if (setup_diagnostics) {
                            chatai::diagnostics::Add("WS_RX", "WARN",
                                "unexpected continuation frame without an active message");
                        }
                        break;
                    }
                } else {
                    if (setup_diagnostics) {
                        chatai::diagnostics::Add("WS_RX", "WARN",
                            "ignored unsupported WebSocket data opcode");
                    }
                    break;
                }
            }

            if (g_incoming_type == IncomingMessageType::kNone ||
                static_cast<size_t>(event->payload_offset) != g_incoming_frame_bytes ||
                g_incoming_message.size() + static_cast<size_t>(event->data_len) >
                    kMaxIncomingMessage) {
                g_incoming_message.clear();
                g_incoming_frame_bytes = 0;
                g_incoming_type = IncomingMessageType::kNone;
                if (setup_diagnostics) {
                    chatai::diagnostics::Add("WS_RX", "WARN",
                        "payload offset/size mismatch; message assembly reset");
                }
                break;
            }

            g_incoming_message.append(event->data_ptr,
                                      static_cast<size_t>(event->data_len));
            g_incoming_frame_bytes += static_cast<size_t>(event->data_len);

            const bool frame_complete =
                g_incoming_frame_bytes == static_cast<size_t>(event->payload_len);
            if (frame_complete) {
                if (event->fin) {
                    if (setup_diagnostics) {
                        char message_detail[96]{};
                        std::snprintf(message_detail, sizeof(message_detail),
                                      "complete %s message bytes=%lu; parsing JSON",
                                      g_incoming_type == IncomingMessageType::kText
                                          ? "text" : "binary",
                                      static_cast<unsigned long>(g_incoming_message.size()));
                        chatai::diagnostics::Add("WS_RX", "OK", message_detail);
                    }
                    // Gemini Live server envelopes are JSON. Try both data frame
                    // types; do not assume a binary frame contains protobuf.
                    HandleServerMessage(g_incoming_message);
                    g_incoming_message.clear();
                    g_incoming_frame_bytes = 0;
                    g_incoming_type = IncomingMessageType::kNone;
                } else {
                    // Preserve message type and bytes for the next continuation.
                    g_incoming_frame_bytes = 0;
                }
            }
            break;
        }

        case WEBSOCKET_EVENT_ERROR:
            // The ERROR event is preliminary; esp_websocket_client fills transport
            // and TLS details before dispatching DISCONNECTED. Defer teardown.
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            g_waiting_setup.store(false);
            g_ready.store(false);
            g_speaking.store(false);
            g_discard_current_response.store(false);
            g_stop_speaker_requested.store(true);
            g_error.store(true);
            g_client_failed.store(true);
            g_audio_epoch.fetch_add(1);
            ClearAudioQueue();
            ReportWebSocketSessionSummary("transport_disconnect");
            ReportWebSocketDisconnectError(event);
            if (!g_connection_failure_reported.exchange(true)) {
                ESP_LOGW(kTag, "Gemini Live WebSocket disconnected");
                chatai::diagnostics::Add("WS", "FAIL", "disconnected");
                NotifyConnectionError("GEMINI", "Mat ket noi WSS");
            }
            break;

        case WEBSOCKET_EVENT_CLOSED:
            g_waiting_setup.store(false);
            g_ready.store(false);
            g_speaking.store(false);
            g_discard_current_response.store(false);
            g_stop_speaker_requested.store(true);
            g_error.store(true);
            g_client_failed.store(true);
            g_audio_epoch.fetch_add(1);
            ClearAudioQueue();
            if (!g_connection_failure_reported.exchange(true)) {
                const int close_code = event != nullptr ? event->close_status_code : 0;
                ESP_LOGW(kTag, "Gemini Live WebSocket closed; code=%d", close_code);
                char detail[96]{};
                std::snprintf(detail, sizeof(detail),
                              "close_code=%d close_source=received_frame_or_client_close",
                              close_code);
                chatai::diagnostics::Add("WS", "FAIL", detail);
                ReportWebSocketSessionSummary("clean_close");
                NotifyConnectionError("GEMINI", "Mat ket noi WSS");
            }
            break;

        default:
            break;
    }
}

void MicrophoneWorker(void*) {
    bool listen_state_initialized = false;
    bool last_should_listen = false;
    bool microphone_read_reported = false;
    bool microphone_read_attempt_recorded = false;
    // Keep the larger 100 ms buffers out of this task's limited FreeRTOS stack.
    static int16_t pcm[kMicChunkSamples]{};
    static unsigned char encoded[kMicEncodedBufferSize]{};
    static char message[kMicMessageBufferSize]{};
    bool microphone_enabled = false;
    bool microphone_error_reported = false;
    bool audio_stream_active = false;
    TickType_t mic_stats_started = 0;
    int32_t mic_min = INT16_MAX;
    int32_t mic_max = INT16_MIN;
    uint64_t mic_sum_abs = 0;
    uint64_t mic_sum_squares = 0;
    int64_t mic_sum = 0;
    uint32_t mic_sample_count = 0;
    uint32_t mic_clipped_count = 0;

    int64_t pipeline_window_started_us = esp_timer_get_time();
    uint64_t pipeline_read_total_us = 0;
    uint64_t pipeline_read_max_us = 0;
    uint64_t pipeline_encode_total_us = 0;
    uint64_t pipeline_encode_max_us = 0;
    uint64_t pipeline_lock_wait_total_us = 0;
    uint64_t pipeline_lock_wait_max_us = 0;
    uint64_t pipeline_send_total_us = 0;
    uint64_t pipeline_send_max_us = 0;
    uint32_t pipeline_read_calls = 0;
    uint32_t pipeline_encode_calls = 0;
    uint32_t pipeline_send_calls = 0;
    uint32_t pipeline_send_failures = 0;
    uint32_t pipeline_iterations = 0;

    while (true) {
        const int64_t pipeline_now_us = esp_timer_get_time();
        if (pipeline_now_us - pipeline_window_started_us >= 5000000) {
            char detail[288]{};
            std::snprintf(detail, sizeof(detail),
                "window_us=%llu loops=%lu read_calls=%lu read_avg_us=%llu read_max_us=%llu "
                "encode_calls=%lu encode_avg_us=%llu encode_max_us=%llu "
                "lock_avg_us=%llu lock_max_us=%llu send_calls=%lu send_avg_us=%llu "
                "send_max_us=%llu send_fail=%lu",
                static_cast<unsigned long long>(pipeline_now_us - pipeline_window_started_us),
                static_cast<unsigned long>(pipeline_iterations),
                static_cast<unsigned long>(pipeline_read_calls),
                static_cast<unsigned long long>(pipeline_read_calls ? pipeline_read_total_us / pipeline_read_calls : 0),
                static_cast<unsigned long long>(pipeline_read_max_us),
                static_cast<unsigned long>(pipeline_encode_calls),
                static_cast<unsigned long long>(pipeline_encode_calls ? pipeline_encode_total_us / pipeline_encode_calls : 0),
                static_cast<unsigned long long>(pipeline_encode_max_us),
                static_cast<unsigned long long>(pipeline_send_calls ? pipeline_lock_wait_total_us / pipeline_send_calls : 0),
                static_cast<unsigned long long>(pipeline_lock_wait_max_us),
                static_cast<unsigned long>(pipeline_send_calls),
                static_cast<unsigned long long>(pipeline_send_calls ? pipeline_send_total_us / pipeline_send_calls : 0),
                static_cast<unsigned long long>(pipeline_send_max_us),
                static_cast<unsigned long>(pipeline_send_failures));
            chatai::diagnostics::Add("MIC_PIPELINE", "INFO", detail);
            pipeline_window_started_us = pipeline_now_us;
            pipeline_read_total_us = pipeline_read_max_us = 0;
            pipeline_encode_total_us = pipeline_encode_max_us = 0;
            pipeline_lock_wait_total_us = pipeline_lock_wait_max_us = 0;
            pipeline_send_total_us = pipeline_send_max_us = 0;
            pipeline_read_calls = pipeline_encode_calls = pipeline_send_calls = 0;
            pipeline_send_failures = pipeline_iterations = 0;
        }
        ++pipeline_iterations;
        const bool should_listen = ShouldListen();
        if (!listen_state_initialized || should_listen != last_should_listen) {
            char detail[144]{};
            std::snprintf(detail, sizeof(detail),
                          "state=%s chat_enabled=%d ptt_pressed=%d gemini_ready=%d stack_watermark=%u",
                          should_listen ? "LISTENING_REQUESTED" : "IDLE",
                          g_chat_enabled.load() ? 1 : 0, g_push_to_talk.load() ? 1 : 0,
                          g_ready.load() ? 1 : 0,
                          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            chatai::diagnostics::Add("LISTEN", "INFO", detail);
            last_should_listen = should_listen;
            listen_state_initialized = true;
        }
        bool transport_ready = g_ready.load() &&
            chatai::network::NetworkManager::IsInternetAvailable();
        {
            std::lock_guard<std::mutex> lock(g_send_mutex);
            transport_ready = transport_ready && g_client != nullptr &&
                esp_websocket_client_is_connected(g_client);
        }
        if (!transport_ready || !should_listen) {
            if (audio_stream_active && transport_ready) {
                static constexpr char kAudioStreamEnd[] =
                    "{\"realtimeInput\":{\"audioStreamEnd\":true}}";
                std::lock_guard<std::mutex> lock(g_send_mutex);
                if (g_client != nullptr && esp_websocket_client_is_connected(g_client)) {
                    const int sent = esp_websocket_client_send_text(g_client, kAudioStreamEnd,
                        sizeof(kAudioStreamEnd) - 1, pdMS_TO_TICKS(1000));
                    if (sent == static_cast<int>(sizeof(kAudioStreamEnd) - 1)) {
                        g_ws_app_tx_count.fetch_add(1);
                        g_ws_last_app_tx_tick.store(static_cast<uint32_t>(xTaskGetTickCount()));
                        g_ws_stream_end_count.fetch_add(1);
                        g_ws_last_stream_end_tick.store(static_cast<uint32_t>(xTaskGetTickCount()));
                    }
                }
            }
            audio_stream_active = false;
            if (microphone_enabled) {
                const esp_err_t stop_result = chatai::audio::EnableMicrophone(false);
                chatai::diagnostics::Add("MIC_STOP",
                    stop_result == ESP_OK ? "OK" : "FAIL",
                    stop_result == ESP_OK ? "I2S RX disabled" : esp_err_to_name(stop_result));
                microphone_enabled = false;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!microphone_enabled) {
            char enable_detail[128]{};
            std::snprintf(enable_detail, sizeof(enable_detail),
                          "attempt=enable_i2s_rx heap_free=%lu stack_watermark=%u",
                          static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            chatai::diagnostics::Add("MIC_ENABLE", "INFO", enable_detail);
            chatai::diagnostics::SetCrashContext("MIC_ENABLE_BEGIN", enable_detail);
            const esp_err_t enable_result = chatai::audio::EnableMicrophone(true);
            if (enable_result != ESP_OK) {
                if (!microphone_error_reported) {
                    ESP_LOGE(kTag, "Could not enable microphone: %s",
                             esp_err_to_name(enable_result));
                    chatai::diagnostics::SetCrashContext("MIC_ENABLE_FAIL", esp_err_to_name(enable_result));
                    chatai::diagnostics::Add("MIC_ENABLE", "FAIL", esp_err_to_name(enable_result));
                    microphone_error_reported = true;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            microphone_enabled = true;
            microphone_error_reported = false;
            microphone_read_reported = false;
            microphone_read_attempt_recorded = false;
            mic_stats_started = xTaskGetTickCount();
            mic_min = INT16_MAX;
            mic_max = INT16_MIN;
            mic_sum_abs = 0;
            mic_sum_squares = 0;
            mic_sum = 0;
            mic_sample_count = 0;
            mic_clipped_count = 0;
            chatai::diagnostics::SetCrashContext("MIC_ENABLED", "I2S RX channel enabled");
            ESP_LOGI(kTag, "Microphone streaming started: PCM16 mono 16 kHz");
            char detail[128]{};
            std::snprintf(detail, sizeof(detail),
                          "I2S RX enabled; heap_free=%lu stack_watermark=%u",
                          static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            chatai::diagnostics::Add("MIC_START", "OK", detail);
        }

        size_t samples_read = 0;
        const int64_t read_started_us = esp_timer_get_time();
        if (!microphone_read_attempt_recorded) {
            chatai::diagnostics::SetCrashContext("MIC_READ_BEGIN", "first I2S RX read");
            microphone_read_attempt_recorded = true;
        }
        const esp_err_t read_result = chatai::audio::ReadMicrophonePcm16(
            pcm, kMicChunkSamples, &samples_read, 100);
        const uint64_t read_elapsed_us = static_cast<uint64_t>(
            std::max<int64_t>(0, esp_timer_get_time() - read_started_us));
        pipeline_read_total_us += read_elapsed_us;
        pipeline_read_max_us = std::max(pipeline_read_max_us, read_elapsed_us);
        ++pipeline_read_calls;
        if (read_result == ESP_ERR_TIMEOUT) continue;
        if (read_result != ESP_OK || samples_read == 0) {
            ESP_LOGW(kTag, "Microphone read failed: %s", esp_err_to_name(read_result));
            if (!microphone_error_reported) {
                chatai::diagnostics::SetCrashContext("MIC_READ_FAIL", esp_err_to_name(read_result));
                chatai::diagnostics::Add("MIC_READ", "FAIL", esp_err_to_name(read_result));
                microphone_error_reported = true;
            }
            chatai::audio::EnableMicrophone(false);
            microphone_enabled = false;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (!microphone_read_reported) {
            char detail[112]{};
            std::snprintf(detail, sizeof(detail),
                          "first_read_samples=%lu stack_watermark=%u",
                          static_cast<unsigned long>(samples_read),
                          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            chatai::diagnostics::Add("MIC_READ", "OK", detail);
            int32_t peak = 0;
            uint64_t absolute_sum = 0;
            for (size_t i = 0; i < samples_read; ++i) {
                const int32_t sample = pcm[i];
                const int32_t magnitude = sample < 0 ? -sample : sample;
                if (magnitude > peak) peak = magnitude;
                absolute_sum += static_cast<uint32_t>(magnitude);
            }
            char level_detail[96]{};
            std::snprintf(level_detail, sizeof(level_detail),
                          "samples=%lu peak=%ld mean_abs=%lu",
                          static_cast<unsigned long>(samples_read),
                          static_cast<long>(peak),
                          static_cast<unsigned long>(absolute_sum / samples_read));
            chatai::diagnostics::Add("MIC_SAMPLE_LEVEL", "INFO", level_detail);
            chatai::diagnostics::SetCrashContext("MIC_READ_OK", level_detail);
            microphone_read_reported = true;
        }

        for (size_t i = 0; i < samples_read; ++i) {
            const int32_t sample = pcm[i];
            const int32_t magnitude = sample < 0 ? -sample : sample;
            mic_min = std::min(mic_min, sample);
            mic_max = std::max(mic_max, sample);
            mic_sum_abs += static_cast<uint32_t>(magnitude);
            mic_sum_squares += static_cast<uint64_t>(magnitude) * magnitude;
            mic_sum += sample;
            if (magnitude >= 32760) ++mic_clipped_count;
            ++mic_sample_count;
        }

        const TickType_t stats_now = xTaskGetTickCount();
        if (mic_sample_count > 0 &&
            stats_now - mic_stats_started >= pdMS_TO_TICKS(5000)) {
            const uint32_t peak = static_cast<uint32_t>(
                std::max(std::abs(mic_min), std::abs(mic_max)));
            const double rms = std::sqrt(
                static_cast<double>(mic_sum_squares) / mic_sample_count);
            const double dc_offset = static_cast<double>(mic_sum) / mic_sample_count;
            const double clipped_percent =
                100.0 * static_cast<double>(mic_clipped_count) / mic_sample_count;
            const uint32_t window_ms = std::max<uint32_t>(
                1, static_cast<uint32_t>(
                    (stats_now - mic_stats_started) * 1000 / configTICK_RATE_HZ));
            const double sample_rate =
                static_cast<double>(mic_sample_count) * 1000.0 / window_ms;
            char level_detail[224]{};
            std::snprintf(level_detail, sizeof(level_detail),
                          "window_ms=%lu samples=%lu rate_sps=%.0f min=%ld max=%ld "
                          "peak=%lu mean_abs=%lu rms=%.1f dc=%.1f clipped=%.2f%%",
                          static_cast<unsigned long>(window_ms),
                          static_cast<unsigned long>(mic_sample_count), sample_rate,
                          static_cast<long>(mic_min), static_cast<long>(mic_max),
                          static_cast<unsigned long>(peak),
                          static_cast<unsigned long>(mic_sum_abs / mic_sample_count),
                          rms, dc_offset, clipped_percent);
            chatai::diagnostics::Add("MIC_LEVEL", "INFO", level_detail);
            mic_stats_started = stats_now;
            mic_min = INT16_MAX;
            mic_max = INT16_MIN;
            mic_sum_abs = 0;
            mic_sum_squares = 0;
            mic_sum = 0;
            mic_sample_count = 0;
            mic_clipped_count = 0;
        }

        size_t encoded_size = 0;
        const int64_t encode_started_us = esp_timer_get_time();
        const int encode_result = mbedtls_base64_encode(
            encoded, sizeof(encoded), &encoded_size,
            reinterpret_cast<const unsigned char*>(pcm), samples_read * sizeof(int16_t));
        const uint64_t encode_elapsed_us = static_cast<uint64_t>(
            std::max<int64_t>(0, esp_timer_get_time() - encode_started_us));
        pipeline_encode_total_us += encode_elapsed_us;
        pipeline_encode_max_us = std::max(pipeline_encode_max_us, encode_elapsed_us);
        ++pipeline_encode_calls;
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

        const int64_t lock_wait_started_us = esp_timer_get_time();
        std::lock_guard<std::mutex> lock(g_send_mutex);
        const uint64_t lock_wait_us = static_cast<uint64_t>(
            std::max<int64_t>(0, esp_timer_get_time() - lock_wait_started_us));
        pipeline_lock_wait_total_us += lock_wait_us;
        pipeline_lock_wait_max_us = std::max(pipeline_lock_wait_max_us, lock_wait_us);
        if (g_ready.load() && g_client != nullptr &&
            esp_websocket_client_is_connected(g_client)) {
            const int64_t send_started_us = esp_timer_get_time();
            const int sent = esp_websocket_client_send_text(
                g_client, message, message_size, pdMS_TO_TICKS(1000));
            const uint64_t send_elapsed_us = static_cast<uint64_t>(
                std::max<int64_t>(0, esp_timer_get_time() - send_started_us));
            pipeline_send_total_us += send_elapsed_us;
            pipeline_send_max_us = std::max(pipeline_send_max_us, send_elapsed_us);
            ++pipeline_send_calls;
            if (sent != message_size) {
                ++pipeline_send_failures;
                ESP_LOGW(kTag, "Microphone audio send was incomplete");
            } else {
                const uint32_t sent_tick = static_cast<uint32_t>(xTaskGetTickCount());
                g_ws_app_tx_count.fetch_add(1);
                g_ws_last_app_tx_tick.store(sent_tick);
                g_ws_audio_tx_count.fetch_add(1);
                g_ws_last_audio_tx_tick.store(sent_tick);
                audio_stream_active = true;
                if (!g_audio_input_reported.exchange(true)) {
                    chatai::diagnostics::SetCrashContext("AUDIO_TX", "first microphone PCM chunk sent");
                    chatai::diagnostics::Add("AUDIO_TX", "OK", "microphone PCM sent to Gemini Live");
                }
            }
        }
    }
}

void SpeakerWorker(void*) {
    AudioChunk* chunk = nullptr;
    bool speaker_enabled = false;
    bool speaker_write_reported = false;
    bool speaker_write_error_reported = false;

    while (true) {
        if (g_stop_speaker_requested.exchange(false)) {
            if (speaker_enabled) {
                chatai::audio::EnableSpeaker(false);
                speaker_enabled = false;
            }
            g_speaking.store(false);
        }
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
            chatai::diagnostics::SetCrashContext("SPEAKER_ENABLE_BEGIN", "enabling I2S TX");
            const esp_err_t enable_result = chatai::audio::EnableSpeaker(true);
            if (enable_result != ESP_OK) {
                g_error.store(true);
                chatai::diagnostics::SetCrashContext("SPEAKER_ENABLE_FAIL", esp_err_to_name(enable_result));
                chatai::diagnostics::Add("SPEAKER_START", "FAIL", esp_err_to_name(enable_result));
                ESP_LOGE(kTag, "Could not enable speaker: %s", esp_err_to_name(enable_result));
                std::free(chunk);
                chunk = nullptr;
                continue;
            }
            speaker_enabled = true;
            chatai::diagnostics::SetCrashContext("SPEAKER_ENABLED", "I2S TX channel enabled");
            if (!g_speaker_started_reported.exchange(true))
                chatai::diagnostics::Add("SPEAKER_START", "OK", "I2S playback started");
        }

        g_speaking.store(true);
        int32_t pcm_peak = 0;
        for (size_t i = 0; i < chunk->sample_count; ++i) {
            const int32_t sample = chunk->samples[i];
            pcm_peak = std::max(pcm_peak, sample < 0 ? -sample : sample);
        }

        size_t samples_written = 0;
        const esp_err_t write_result = chatai::audio::WriteSpeakerPcm16(
            chunk->samples, chunk->sample_count, &samples_written, 100);
        const size_t requested_bytes = chunk->sample_count * sizeof(int32_t);
        const size_t written_bytes = samples_written * sizeof(int32_t);
        const int output_volume = chatai::audio::GetOutputVolume();
        if (write_result != ESP_OK || samples_written != chunk->sample_count) {
            char detail[160]{};
            std::snprintf(detail, sizeof(detail),
                          "result=%s samples=%lu/%lu bytes=%lu/%lu volume=%d peak=%ld",
                          esp_err_to_name(write_result),
                          static_cast<unsigned long>(samples_written),
                          static_cast<unsigned long>(chunk->sample_count),
                          static_cast<unsigned long>(written_bytes),
                          static_cast<unsigned long>(requested_bytes),
                          output_volume, static_cast<long>(pcm_peak));
            ESP_LOGW(kTag, "Speaker write incomplete: %s", detail);
            if (!speaker_write_error_reported) {
                chatai::diagnostics::Add("SPEAKER_WRITE", "FAIL", detail);
                speaker_write_error_reported = true;
            }
        } else if (!speaker_write_reported) {
            char detail[160]{};
            std::snprintf(detail, sizeof(detail),
                          "samples=%lu bytes=%lu volume=%d pcm_peak=%ld",
                          static_cast<unsigned long>(samples_written),
                          static_cast<unsigned long>(written_bytes),
                          output_volume, static_cast<long>(pcm_peak));
            chatai::diagnostics::SetCrashContext("SPEAKER_WRITE_OK", detail);
            chatai::diagnostics::Add("SPEAKER_WRITE", "OK", detail);
            speaker_write_reported = true;
            speaker_write_error_reported = false;
        }
        std::free(chunk);
        chunk = nullptr;
    }
}

void GeminiWorker(void*) {
    uint32_t reconnect_delay_ms = 1000;
    while (true) {
        const bool manual_restart = g_restart_requested.exchange(false);
        if (manual_restart) {
            reconnect_delay_ms = 1000;
            g_retry_blocked.store(false);
            g_tls_fatal.store(false);
            g_error.store(false);
            chatai::diagnostics::Add("WS", "INFO", "manual reconnect requested");
        }

        esp_websocket_client_handle_t client = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_send_mutex);
            client = g_client;
        }

        if (client != nullptr) {
            if (g_ready.load()) reconnect_delay_ms = 1000;
            const bool connection_failed = g_client_failed.exchange(false);
            if (manual_restart || connection_failed) {
                g_client_generation.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lock(g_send_mutex);
                    if (g_client == client) g_client = nullptr;
                }
                {
                    std::lock_guard<std::mutex> event_lock(g_event_mutex);
                }
                const esp_err_t stop_result = esp_websocket_client_stop(client);
                esp_websocket_client_destroy(client);
                g_ready.store(false);
                g_waiting_setup.store(false);
                g_speaking.store(false);
                g_audio_epoch.fetch_add(1);
                ClearAudioQueue();
                g_client_failed.store(false);

                if (stop_result != ESP_OK) {
                    ESP_LOGW(kTag, "Gemini client stop returned: %s",
                             esp_err_to_name(stop_result));
                }
                if (!manual_restart) {
                    if (!g_retry_blocked.load() && !g_tls_fatal.load()) {
                        char retry_detail[96]{};
                        std::snprintf(retry_detail, sizeof(retry_detail),
                                      "transport/session failure; retry_in_ms=%lu",
                                      static_cast<unsigned long>(reconnect_delay_ms));
                        chatai::diagnostics::Add("RETRY", "SCHEDULED", retry_detail);
                        vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));
                        reconnect_delay_ms = std::min<uint32_t>(
                            reconnect_delay_ms * 2, 30000);
                    } else {
                        chatai::diagnostics::Add("RETRY", "PAUSED",
                            g_tls_fatal.load()
                                ? "TLS verification failure requires manual recovery"
                                : "retry blocked by authentication or protocol error");
                    }
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
                    g_client_failed.store(true);
                    g_connection_failure_reported.store(true);
                    ESP_LOGE(kTag, "Gemini Live did not send setupComplete within 20 seconds");
                    chatai::diagnostics::Add("SETUP", "FAIL", "setupComplete timeout after 20 seconds");
                    NotifyConnectionError("GEMINI", "Qua han setup");
                    esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        if (g_retry_blocked.load()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
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
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        g_missing_key_reported = false;
        if (!EnsureSystemTime()) {
            std::fill(api_key.begin(), api_key.end(), '\0');
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Run one independent TLS handshake before WSS. This separates CA/hostname
        // verification failures from WebSocket-specific handshake failures.
        if (!g_tls_probe_completed) {
            g_tls_probe_completed = true;
            ProbeGeminiTls();
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
        config.crt_bundle_attach = AttachGeminiCertificateBundle;
        config.buffer_size = 6144;
        config.task_stack = 6144;
        config.task_prio = 5;
        config.network_timeout_ms = 15000;
        config.reconnect_timeout_ms = 5000;
        config.ping_interval_sec = 20;
        config.disable_auto_reconnect = true;
        config.enable_close_reconnect = false;

        esp_websocket_client_handle_t new_client = esp_websocket_client_init(&config);
        if (new_client == nullptr) {
            g_error.store(true);
            ESP_LOGE(kTag, "Could not initialize Gemini Live WebSocket client");
            chatai::diagnostics::Add("WS", "FAIL", "esp_websocket_client_init returned null");
            char retry_detail[96]{};
            std::snprintf(retry_detail, sizeof(retry_detail),
                          "client initialization failed; retry_in_ms=%lu",
                          static_cast<unsigned long>(reconnect_delay_ms));
            chatai::diagnostics::Add("RETRY", "SCHEDULED", retry_detail);
            vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));
            reconnect_delay_ms = std::min<uint32_t>(reconnect_delay_ms * 2, 30000);
            continue;
        }

        g_client_failed.store(false);
        g_connection_failure_reported.store(false);
        const uint32_t generation = g_client_generation.fetch_add(1) + 1;
        {
            std::lock_guard<std::mutex> lock(g_send_mutex);
            g_client = new_client;
        }
        esp_err_t result = esp_websocket_register_events(
            new_client, WEBSOCKET_EVENT_ANY, WebSocketEventHandler,
            reinterpret_cast<void*>(static_cast<uintptr_t>(generation)));
        if (result == ESP_OK) result = esp_websocket_client_start(new_client);

        if (result != ESP_OK) {
            g_error.store(true);
            ESP_LOGE(kTag, "Could not start Gemini Live WebSocket: %s",
                     esp_err_to_name(result));
            chatai::diagnostics::Add("WS", "FAIL", esp_err_to_name(result));
            g_client_generation.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(g_send_mutex);
                if (g_client == new_client) g_client = nullptr;
            }
            {
                std::lock_guard<std::mutex> event_lock(g_event_mutex);
            }
            esp_websocket_client_destroy(new_client);
            char retry_detail[96]{};
            std::snprintf(retry_detail, sizeof(retry_detail),
                          "client start failed; retry_in_ms=%lu",
                          static_cast<unsigned long>(reconnect_delay_ms));
            chatai::diagnostics::Add("RETRY", "SCHEDULED", retry_detail);
            vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));
            reconnect_delay_ms = std::min<uint32_t>(reconnect_delay_ms * 2, 30000);
            continue;
        }

        ESP_LOGI(kTag, "Gemini Live connection started; waiting for setupComplete");
        if (!g_model_reported.exchange(true))
            chatai::diagnostics::Add("MODEL", "INFO", "models/gemini-3.8-live; response modality AUDIO");
        if (!g_ws_start_reported.exchange(true))
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
        SpeakerWorker, "chatai_spk", 6144, nullptr, 5, nullptr);
    if (result == pdPASS) {
        result = xTaskCreate(MicrophoneWorker, "chatai_mic", 8192, nullptr, 5, nullptr);
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

bool IsTlsFatal() {
    return g_tls_fatal.load();
}

bool IsSpeaking() {
    return g_speaking.load();
}

void AbortSpeaking() {
    if (!g_speaking.exchange(false)) return;
    g_discard_current_response.store(true);
    g_audio_epoch.fetch_add(1);
    ClearAudioQueue();
    g_stop_speaker_requested.store(true);
    chatai::diagnostics::Add("PLAYBACK", "STOPPED",
        "local playback stopped by BOOT; server generation may continue until turnComplete or incoming speech interruption");
}

bool HasError() {
    return g_error.load();
}

void SetChatEnabled(bool enabled) {
    const bool was_enabled = g_chat_enabled.exchange(enabled);
    if (enabled && !was_enabled && !g_ready.load() &&
        !g_waiting_setup.load() &&
        (g_client_failed.load() || g_retry_blocked.load())) {
        chatai::diagnostics::Add("CHAT", "INFO",
            "continuous listening requested after Gemini session ended; reconnecting");
        RequestReconnect();
    }
}
bool IsChatEnabled() { return g_chat_enabled.load(); }
void SetPushToTalk(bool pressed) {
    const bool was_pressed = g_push_to_talk.exchange(pressed);
    if (pressed && !was_pressed && !g_ready.load() &&
        !g_waiting_setup.load() &&
        (g_client_failed.load() || g_retry_blocked.load())) {
        chatai::diagnostics::Add("PTT", "INFO",
            "press detected after Gemini session failure; requesting one reconnect");
        RequestReconnect();
    }
}
bool IsListening() { return ShouldListen(); }

void RequestReconnect() {
    g_retry_blocked.store(false);
    g_tls_fatal.store(false);
    g_connection_notification_active.store(false);
    g_restart_requested.store(true);
    g_ready.store(false);
    g_error.store(false);
    g_audio_epoch.fetch_add(1);
    ClearAudioQueue();
}

}  // namespace chatai::gemini
