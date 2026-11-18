#include "gemini_live_protocol.h"
#include "board.h"
#include "settings.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#define TAG "GeminiLive"
namespace {
constexpr char kUrl[] = "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent";
constexpr size_t kMaxMessage = 256 * 1024;
constexpr size_t kGeminiOutputSampleRate = 24000;
constexpr size_t kGeminiOutputChunkDurationMs = 60;
constexpr size_t kPcmDecodeChunkSamples =
    kGeminiOutputChunkDurationMs * kGeminiOutputSampleRate / 1000;
static_assert(kPcmDecodeChunkSamples == 1440, "Gemini PCM chunk sizing changed");
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::atomic<uint32_t> g_microphone_chunks_sent{0};
std::atomic<uint32_t> g_response_audio_chunks_received{0};
std::atomic<bool> g_session_connected{false};
std::atomic<bool> g_microphone_streaming{false};
std::mutex g_diagnostics_mutex;
std::string g_last_error;

constexpr uint32_t kDiagnosticHistoryMagic = 0x47444832;
constexpr size_t kDiagnosticHistoryCapacity = 24;
struct DiagnosticHistoryEvent {
    uint32_t boot;
    uint32_t uptime_ms;
    char message[72];
};
struct DiagnosticHistoryStore {
    uint32_t magic;
    uint32_t boot_count;
    uint32_t write_index;
    uint32_t total_events;
    uint32_t current_session_started;
    uint32_t current_mic_chunks;
    uint32_t current_response_chunks;
    uint32_t current_session_connected;
    uint32_t current_mic_streaming;
    char current_last_error[72];
    uint32_t previous_valid;
    uint32_t previous_boot;
    uint32_t previous_reset_reason;
    uint32_t previous_mic_chunks;
    uint32_t previous_response_chunks;
    uint32_t previous_session_connected;
    uint32_t previous_mic_streaming;
    char previous_last_error[72];
    DiagnosticHistoryEvent events[kDiagnosticHistoryCapacity];
};
__NOINIT_ATTR DiagnosticHistoryStore g_diagnostic_history;
portMUX_TYPE g_diagnostic_history_mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> g_diagnostic_history_initialized{false};

void EnsureDiagnosticHistory() {
    if (g_diagnostic_history_initialized.load()) return;
    char boot_message[72] = {};
    const uint32_t uptime_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const uint32_t reset_reason = static_cast<uint32_t>(esp_reset_reason());
    std::snprintf(boot_message, sizeof(boot_message), "Boot started; reset_reason=%lu",
                  static_cast<unsigned long>(reset_reason));
    portENTER_CRITICAL(&g_diagnostic_history_mux);
    if (!g_diagnostic_history_initialized.load()) {
        if (g_diagnostic_history.magic != kDiagnosticHistoryMagic ||
            g_diagnostic_history.write_index >= kDiagnosticHistoryCapacity) {
            std::memset(&g_diagnostic_history, 0, sizeof(g_diagnostic_history));
            g_diagnostic_history.magic = kDiagnosticHistoryMagic;
        }
        if (g_diagnostic_history.current_session_started) {
            g_diagnostic_history.previous_valid = 1;
            g_diagnostic_history.previous_boot = g_diagnostic_history.boot_count;
            g_diagnostic_history.previous_reset_reason = reset_reason;
            g_diagnostic_history.previous_mic_chunks = g_diagnostic_history.current_mic_chunks;
            g_diagnostic_history.previous_response_chunks = g_diagnostic_history.current_response_chunks;
            g_diagnostic_history.previous_session_connected = g_diagnostic_history.current_session_connected;
            g_diagnostic_history.previous_mic_streaming = g_diagnostic_history.current_mic_streaming;
            std::memcpy(g_diagnostic_history.previous_last_error,
                        g_diagnostic_history.current_last_error,
                        sizeof(g_diagnostic_history.previous_last_error));
        }
        ++g_diagnostic_history.boot_count;
        g_diagnostic_history.current_session_started = 0;
        g_diagnostic_history.current_mic_chunks = 0;
        g_diagnostic_history.current_response_chunks = 0;
        g_diagnostic_history.current_session_connected = 0;
        g_diagnostic_history.current_mic_streaming = 0;
        g_diagnostic_history.current_last_error[0] = '\0';
        auto& event = g_diagnostic_history.events[g_diagnostic_history.write_index];
        event.boot = g_diagnostic_history.boot_count;
        event.uptime_ms = uptime_ms;
        std::memcpy(event.message, boot_message, sizeof(event.message));
        g_diagnostic_history.write_index =
            (g_diagnostic_history.write_index + 1) % kDiagnosticHistoryCapacity;
        ++g_diagnostic_history.total_events;
        g_diagnostic_history_initialized.store(true);
    }
    portEXIT_CRITICAL(&g_diagnostic_history_mux);
}

void RecordDiagnosticEvent(const char* format, ...) {
    EnsureDiagnosticHistory();
    char message[72] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    const uint32_t uptime_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&g_diagnostic_history_mux);
    if (g_diagnostic_history.magic == kDiagnosticHistoryMagic) {
        auto& event = g_diagnostic_history.events[g_diagnostic_history.write_index];
        event.boot = g_diagnostic_history.boot_count;
        event.uptime_ms = uptime_ms;
        std::memcpy(event.message, message, sizeof(event.message));
        g_diagnostic_history.write_index =
            (g_diagnostic_history.write_index + 1) % kDiagnosticHistoryCapacity;
        ++g_diagnostic_history.total_events;
    }
    portEXIT_CRITICAL(&g_diagnostic_history_mux);
}

void BeginPersistedGeminiSession() {
    EnsureDiagnosticHistory();
    portENTER_CRITICAL(&g_diagnostic_history_mux);
    g_diagnostic_history.current_session_started = 1;
    g_diagnostic_history.current_mic_chunks = 0;
    g_diagnostic_history.current_response_chunks = 0;
    g_diagnostic_history.current_session_connected = 0;
    g_diagnostic_history.current_mic_streaming = 0;
    g_diagnostic_history.current_last_error[0] = '\0';
    portEXIT_CRITICAL(&g_diagnostic_history_mux);
}

void SetPersistedGeminiState(bool connected, bool microphone_streaming) {
    EnsureDiagnosticHistory();
    portENTER_CRITICAL(&g_diagnostic_history_mux);
    g_diagnostic_history.current_session_connected = connected ? 1 : 0;
    g_diagnostic_history.current_mic_streaming = microphone_streaming ? 1 : 0;
    portEXIT_CRITICAL(&g_diagnostic_history_mux);
}

void SetPersistedGeminiCounters(uint32_t microphone_chunks, uint32_t response_chunks) {
    EnsureDiagnosticHistory();
    portENTER_CRITICAL(&g_diagnostic_history_mux);
    g_diagnostic_history.current_mic_chunks = microphone_chunks;
    g_diagnostic_history.current_response_chunks = response_chunks;
    portEXIT_CRITICAL(&g_diagnostic_history_mux);
}

void SetPersistedGeminiError(const char* message) {
    EnsureDiagnosticHistory();
    char safe_message[72] = {};
    if (message) std::snprintf(safe_message, sizeof(safe_message), "%s", message);
    portENTER_CRITICAL(&g_diagnostic_history_mux);
    std::memcpy(g_diagnostic_history.current_last_error, safe_message, sizeof(safe_message));
    portEXIT_CRITICAL(&g_diagnostic_history_mux);
}

std::string GetPreviousGeminiSessionSummary() {
    EnsureDiagnosticHistory();
    uint32_t valid = 0, boot = 0, reason = 0, mic = 0, response = 0, connected = 0, streaming = 0;
    char last_error[72] = {};
    portENTER_CRITICAL(&g_diagnostic_history_mux);
    valid = g_diagnostic_history.previous_valid;
    boot = g_diagnostic_history.previous_boot;
    reason = g_diagnostic_history.previous_reset_reason;
    mic = g_diagnostic_history.previous_mic_chunks;
    response = g_diagnostic_history.previous_response_chunks;
    connected = g_diagnostic_history.previous_session_connected;
    streaming = g_diagnostic_history.previous_mic_streaming;
    std::memcpy(last_error, g_diagnostic_history.previous_last_error, sizeof(last_error));
    portEXIT_CRITICAL(&g_diagnostic_history_mux);
    if (!valid) return "Chưa có phiên Gemini trước đó được lưu.";
    const char* reset_reason = "UNKNOWN";
    switch (static_cast<esp_reset_reason_t>(reason)) {
        case ESP_RST_POWERON: reset_reason = "POWER ON"; break;
        case ESP_RST_EXT: reset_reason = "EXTERNAL"; break;
        case ESP_RST_SW: reset_reason = "SOFTWARE"; break;
        case ESP_RST_PANIC: reset_reason = "PANIC"; break;
        case ESP_RST_INT_WDT: reset_reason = "INT WDT"; break;
        case ESP_RST_TASK_WDT: reset_reason = "TASK WDT"; break;
        case ESP_RST_WDT: reset_reason = "WDT"; break;
        case ESP_RST_DEEPSLEEP: reset_reason = "DEEPSLEEP"; break;
        case ESP_RST_BROWNOUT: reset_reason = "BROWNOUT"; break;
        case ESP_RST_SDIO: reset_reason = "SDIO"; break;
        default: break;
    }
    char summary[256] = {};
    std::snprintf(summary, sizeof(summary),
                  "Phiên Gemini gần nhất: boot #%lu; reset=%s; mic chunks=%lu; response audio chunks=%lu; connected trước reset=%s; mic streaming trước reset=%s; lỗi cuối=%s",
                  static_cast<unsigned long>(boot), reset_reason,
                  static_cast<unsigned long>(mic), static_cast<unsigned long>(response),
                  connected ? "yes" : "no", streaming ? "yes" : "no",
                  last_error[0] ? last_error : "None");
    return summary;
}

std::string GetDiagnosticHistory() {
    EnsureDiagnosticHistory();
    uint32_t write_index = 0;
    uint32_t total_events = 0;
    portENTER_CRITICAL(&g_diagnostic_history_mux);
    write_index = g_diagnostic_history.write_index;
    total_events = g_diagnostic_history.total_events;
    portEXIT_CRITICAL(&g_diagnostic_history_mux);

    const size_t count = std::min(static_cast<size_t>(total_events), kDiagnosticHistoryCapacity);
    const size_t start = (write_index + kDiagnosticHistoryCapacity - count) %
                         kDiagnosticHistoryCapacity;
    std::string history = "Retained event history (latest 24 events; no-init RAM, not power-loss storage):\n";
    for (size_t i = 0; i < count; ++i) {
        DiagnosticHistoryEvent event = {};
        const size_t index = (start + i) % kDiagnosticHistoryCapacity;
        portENTER_CRITICAL(&g_diagnostic_history_mux);
        std::memcpy(&event, &g_diagnostic_history.events[index], sizeof(event));
        portEXIT_CRITICAL(&g_diagnostic_history_mux);

        char line[112] = {};
        std::snprintf(line, sizeof(line), "Boot %lu +%lu ms: %s\n",
                      static_cast<unsigned long>(event.boot),
                      static_cast<unsigned long>(event.uptime_ms), event.message);
        history += line;
    }
    return history;
}

std::string Encode64(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(4 * ((size + 2) / 3));
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t a = data[i], b = i + 1 < size ? data[i + 1] : 0, c = i + 2 < size ? data[i + 2] : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out.push_back(kB64[(v >> 18) & 63]); out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(i + 1 < size ? kB64[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? kB64[v & 63] : '=');
    }
    return out;
}
template <typename Consumer>
bool Decode64InChunks(const char* data, size_t size, Consumer&& consume) {
    if (!data || size == 0 || size > kMaxMessage || size % 4 != 0) return false;

    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::vector<int16_t> samples;
    samples.reserve(kPcmDecodeChunkSamples);
    bool has_low_byte = false;
    uint8_t low_byte = 0;

    auto append_byte = [&](uint8_t byte) -> bool {
        if (!has_low_byte) {
            low_byte = byte;
            has_low_byte = true;
            return true;
        }

        const uint16_t raw = static_cast<uint16_t>(low_byte) |
                             (static_cast<uint16_t>(byte) << 8);
        samples.push_back(static_cast<int16_t>(raw));
        has_low_byte = false;

        if (samples.size() == kPcmDecodeChunkSamples) {
            if (!consume(std::move(samples))) return false;
            samples.clear();
            samples.reserve(kPcmDecodeChunkSamples);
        }
        return true;
    };

    for (size_t i = 0; i < size; i += 4) {
        const bool pad2 = data[i + 2] == '=';
        const bool pad3 = data[i + 3] == '=';
        if ((pad2 && !pad3) || ((pad2 || pad3) && i + 4 != size)) return false;

        const int a = value(data[i]);
        const int b = value(data[i + 1]);
        const int c = pad2 ? 0 : value(data[i + 2]);
        const int d = pad3 ? 0 : value(data[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;

        const uint32_t decoded = (static_cast<uint32_t>(a) << 18) |
                                 (static_cast<uint32_t>(b) << 12) |
                                 (static_cast<uint32_t>(c) << 6) |
                                 static_cast<uint32_t>(d);
        if (!append_byte(static_cast<uint8_t>(decoded >> 16))) return false;
        if (!pad2 && !append_byte(static_cast<uint8_t>(decoded >> 8))) return false;
        if (!pad3 && !append_byte(static_cast<uint8_t>(decoded))) return false;
    }

    if (has_low_byte) return false;
    return samples.empty() || consume(std::move(samples));
}
std::string PrintJson(cJSON* root) {
    char* p = cJSON_PrintUnformatted(root); if (!p) return {};
    std::string s(p); cJSON_free(p); return s;
}
}
GeminiLiveProtocol::GeminiLiveProtocol(AudioService& audio) : audio_service_(audio) {
    EnsureDiagnosticHistory();
    RecordDiagnosticEvent("Gemini protocol constructed");
    event_group_ = xEventGroupCreate();
}
GeminiLiveProtocol::~GeminiLiveProtocol() { CloseAudioChannel(false); if (event_group_) vEventGroupDelete(event_group_); }
GeminiLiveProtocol::Diagnostics GeminiLiveProtocol::GetDiagnostics() {
    Diagnostics result;
    result.microphone_chunks_sent = g_microphone_chunks_sent.load();
    result.response_audio_chunks_received = g_response_audio_chunks_received.load();
    result.session_connected = g_session_connected.load();
    result.microphone_streaming = g_microphone_streaming.load();
    {
        std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
        result.last_error = g_last_error;
    }
    result.event_history = GetDiagnosticHistory();
    result.previous_session_summary = GetPreviousGeminiSessionSummary();
    return result;
}
void GeminiLiveProtocol::InitializeDiagnosticHistory() {
    EnsureDiagnosticHistory();
    RecordDiagnosticEvent("Board diagnostics initialized");
}
bool GeminiLiveProtocol::Start() { return true; }
bool GeminiLiveProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) { (void)packet; return false; }
bool GeminiLiveProtocol::SendLiveVoiceNotification(const std::string& text) {
    // A one-shot notification may be sent while the device is idle. Do not
    // require microphone streaming; the existing Live session returns audio.
    if (text.empty() || output_turn_started_.load() || !IsAudioChannelOpened()) {
        return false;
    }

    // Realtime text is not added to chat history and, unlike clientContent,
    // does not interrupt a model response already in progress.
    cJSON* root = cJSON_CreateObject();
    cJSON* input = cJSON_AddObjectToObject(root, "realtimeInput");
    cJSON_AddStringToObject(input, "text", text.c_str());
    std::string message = PrintJson(root);
    cJSON_Delete(root);
    return !message.empty() && SendText(message);
}
bool GeminiLiveProtocol::IsAudioChannelOpened() const { return websocket_ && websocket_->IsConnected() && setup_complete_ && !error_occurred_; }
bool GeminiLiveProtocol::SendText(const std::string& text) {
    if (!IsAudioChannelOpened()) return false;
    if (!websocket_->Send(text)) { Fail("Gemini Live send failed"); return false; }
    return true;
}
bool GeminiLiveProtocol::SendSetup() {
    cJSON* root = cJSON_CreateObject(); cJSON* setup = cJSON_AddObjectToObject(root, "setup");
    cJSON_AddStringToObject(setup, "model", model_.c_str());
    cJSON* gen = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON* modes = cJSON_AddArrayToObject(gen, "responseModalities"); cJSON_AddItemToArray(modes, cJSON_CreateString("AUDIO"));
    cJSON* speech = cJSON_AddObjectToObject(gen, "speechConfig");
    cJSON_AddStringToObject(speech, "languageCode", language_code_.c_str());
    cJSON* voice = cJSON_AddObjectToObject(speech, "voiceConfig");
    cJSON* prebuilt = cJSON_AddObjectToObject(voice, "prebuiltVoiceConfig");
    cJSON_AddStringToObject(prebuilt, "voiceName", voice_name_.c_str());
    cJSON* instruction = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON* parts = cJSON_AddArrayToObject(instruction, "parts"); cJSON* part = cJSON_CreateObject();
    const std::string instruction_text = "You are " + bot_name_ + ". " + system_instruction_;
    cJSON_AddStringToObject(part, "text", instruction_text.c_str()); cJSON_AddItemToArray(parts, part);
    cJSON_AddObjectToObject(setup, "inputAudioTranscription"); cJSON_AddObjectToObject(setup, "outputAudioTranscription");
    std::string msg = PrintJson(root); cJSON_Delete(root); return !msg.empty() && websocket_->Send(msg);
}
bool GeminiLiveProtocol::OpenAudioChannel() {
    BeginPersistedGeminiSession();
    RecordDiagnosticEvent("Gemini connect requested");
    g_microphone_chunks_sent = 0;
    g_response_audio_chunks_received = 0;
    g_session_connected = false;
    g_microphone_streaming = false;
    { std::lock_guard<std::mutex> lock(g_diagnostics_mutex); g_last_error.clear(); }
    error_occurred_ = false;
    Settings cfg("gemini", false);
    if (!cfg.GetBool("enabled", false)) {
        Fail("Gemini Live is disabled; enable it in the local Web Admin");
        return false;
    }
    api_key_ = cfg.GetString("api_key"); model_ = cfg.GetString("model", "gemini-3.8-live");
    system_instruction_ = cfg.GetString("system_prompt", "A helpful Vietnamese voice assistant. Speak naturally in Vietnamese unless the user asks for another language. Keep replies concise and conversational.");
    bot_name_ = cfg.GetString("bot_name", "ChatAI");
    voice_name_ = cfg.GetString("voice_name", "Puck");
    language_code_ = cfg.GetString("language_code", "vi-VN");
    if (api_key_.empty()) { Fail("Gemini API key is not configured in local device settings"); return false; }
    if (model_.rfind("models/", 0) != 0) model_ = "models/" + model_;
    error_occurred_ = false; closing_ = false; listening_ = false; output_turn_started_ = false; turn_complete_pending_ = false; resume_listening_after_playback_ = false; setup_complete_ = false; last_server_error_.clear();
    xEventGroupClearBits(event_group_, kSetupComplete | kSetupFailed | kReceiveFrameAvailable);
    receive_frames_dropped_.store(0);
    auto* network = Board::GetInstance().GetNetwork();
    if (!network) { Fail("Gemini Live network interface is unavailable"); return false; }
    auto socket = network->CreateWebSocket(1);
    if (!socket) { Fail("Failed to create Gemini Live WebSocket"); return false; }
    websocket_ = std::shared_ptr<WebSocket>(std::move(socket));
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 2 * 16384 + 10000 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 16384) {
        ESP_LOGE(TAG, "Low heap, skip");
        Fail("Gemini Live has insufficient heap for WebSocket receive buffers");
        return false;
    }
    websocket_->SetReceiveBufferSize(16384);
    websocket_->OnData([this](const char* data, size_t size, bool binary) {
        // Never parse JSON, decode Base64, allocate PCM vectors or wait for the
        // speaker queue in the WebSocket callback. Copy into the fixed ring only.
        QueueIncomingFrame(data, size, binary);
    });
    websocket_->OnError([this](const NetworkError&) {
        if (!closing_) {
            RecordDiagnosticEvent("WebSocket transport error");
            if (!setup_complete_) xEventGroupSetBits(event_group_, kSetupFailed);
            Fail("Gemini Live WebSocket transport error");
        }
    });
    websocket_->OnDisconnected([this]() {
        RecordDiagnosticEvent("WebSocket disconnected");
        SetPersistedGeminiState(false, false);
        if (!setup_complete_) xEventGroupSetBits(event_group_, kSetupFailed);
        setup_complete_ = false; listening_ = false; g_session_connected = false; g_microphone_streaming = false; audio_service_.EnablePcmBridge(false); audio_service_.EnablePcmPlaybackMode(false); audio_service_.ClearPcmPlayback();
        if (!closing_ && channel_open_notified_.exchange(false) && on_audio_channel_closed_) on_audio_channel_closed_();
    });
    // The Gemini Live WebSocket endpoint authenticates with the API key in the query string.
    // Keep the key out of logs; do not log this URL.
    const std::string url = std::string(kUrl) + "?key=" + api_key_;
    if (auto connected = websocket_->Connect(url.c_str()); !connected) { Fail("Could not connect to Gemini Live"); CloseAudioChannel(false); return false; }
    RecordDiagnosticEvent("WebSocket connected; setup sent next");
    if (!SendSetup()) { Fail("Could not send Gemini Live setup"); CloseAudioChannel(false); return false; }
    const TickType_t setup_timeout = pdMS_TO_TICKS(15000);
    const TickType_t setup_started = xTaskGetTickCount();
    while (!setup_complete_.load() && !error_occurred_) {
        const TickType_t elapsed = xTaskGetTickCount() - setup_started;
        if (elapsed >= setup_timeout) break;
        const EventBits_t bits = xEventGroupWaitBits(
            event_group_, kSetupComplete | kSetupFailed | kReceiveFrameAvailable,
            pdTRUE, pdFALSE, setup_timeout - elapsed);
        if (bits & kReceiveFrameAvailable) ProcessIncomingFrames();
        if (bits & kSetupFailed) break;
    }
    if (!setup_complete_.load()) {
        Fail(last_server_error_.empty() ? "Gemini Live setup timed out" : last_server_error_);
        CloseAudioChannel(false);
        return false;
    }
    setup_complete_ = true; g_session_connected = true;
    SetPersistedGeminiState(true, false);
    RecordDiagnosticEvent("Gemini session connected");
    audio_service_.EnablePcmPlaybackMode(true); last_incoming_time_ = std::chrono::steady_clock::now();
    channel_open_notified_ = true; if (on_connected_) on_connected_(); if (on_audio_channel_opened_) on_audio_channel_opened_();
    ESP_LOGI(TAG, "Gemini Live session ready; model=%s", model_.c_str()); return true;
}
void GeminiLiveProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    if (g_session_connected.load() || websocket_) RecordDiagnosticEvent("Gemini channel closing");
    SetPersistedGeminiState(false, false);
    closing_ = true; listening_ = false; setup_complete_ = false; turn_complete_pending_ = false; resume_listening_after_playback_ = false; g_session_connected = false; g_microphone_streaming = false;
    audio_service_.EnablePcmBridge(false); audio_service_.EnablePcmPlaybackMode(false); audio_service_.ClearPcmPlayback();
    if (channel_open_notified_.exchange(false) && on_audio_channel_closed_) on_audio_channel_closed_();
    if (websocket_) { websocket_->Close(); websocket_.reset(); }
    portENTER_CRITICAL(&receive_mux_);
    for (auto& frame : receive_frames_) {
        frame.state = 0;
        frame.size = 0;
    }
    receive_read_index_ = 0;
    receive_write_index_ = 0;
    portEXIT_CRITICAL(&receive_mux_);
    api_key_.clear();
}
void GeminiLiveProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
    if (IsAudioChannelOpened() && !output_turn_started_ && !resume_listening_after_playback_) {
        audio_service_.EnablePcmBridge(true);
        listening_ = true;
        g_microphone_streaming = true;
        SetPersistedGeminiState(true, true);
        RecordDiagnosticEvent("Microphone streaming started");
    }
}
void GeminiLiveProtocol::SendStopListening() {
    if (!IsAudioChannelOpened()) { listening_ = false; return; }
    ProcessPending(); listening_ = false; resume_listening_after_playback_ = false; g_microphone_streaming = false;
    SetPersistedGeminiState(g_session_connected.load(), false);
    RecordDiagnosticEvent("Microphone streaming stopped; sent=%lu", static_cast<unsigned long>(g_microphone_chunks_sent.load()));
    audio_service_.EnablePcmBridge(false); SendAudioStreamEnd();
}
void GeminiLiveProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    turn_complete_pending_ = false;
    audio_service_.ClearPcmPlayback();
    FinishOutputTurn();
}
void GeminiLiveProtocol::SendWakeWordDetected(const std::string& word) { (void)word; }
bool GeminiLiveProtocol::SendAudioChunk(const int16_t* samples, size_t count) {
    if (!samples || !count || !IsAudioChannelOpened()) return false;
    std::string b64 = Encode64(reinterpret_cast<const uint8_t*>(samples), count * sizeof(int16_t)); if (b64.empty()) return false;
    cJSON* root = cJSON_CreateObject(); cJSON* realtime = cJSON_AddObjectToObject(root, "realtimeInput");
    cJSON* audio = cJSON_AddObjectToObject(realtime, "audio"); cJSON_AddStringToObject(audio, "mimeType", "audio/pcm;rate=16000");
    cJSON_AddStringToObject(audio, "data", b64.c_str()); std::string msg = PrintJson(root); cJSON_Delete(root);
    if (msg.empty() || !websocket_->Send(msg)) return false;
    const uint32_t sent = ++g_microphone_chunks_sent;
    SetPersistedGeminiCounters(sent, g_response_audio_chunks_received.load());
    if (sent == 1 || sent % 100 == 0) {
        RecordDiagnosticEvent("Microphone audio chunks sent=%lu", static_cast<unsigned long>(sent));
    }
    return true;
}
bool GeminiLiveProtocol::SendAudioStreamEnd() {
    cJSON* root = cJSON_CreateObject(); cJSON* realtime = cJSON_AddObjectToObject(root, "realtimeInput"); cJSON_AddBoolToObject(realtime, "audioStreamEnd", true);
    std::string msg = PrintJson(root); cJSON_Delete(root); return !msg.empty() && SendText(msg);
}
void GeminiLiveProtocol::ProcessPending() {
    // Parse/decode queued server frames on the application protocol task,
    // never on the WebSocket receive callback.
    ProcessIncomingFrames();
    // Gemini turnComplete means generation ended, not that the speaker finished.
    // Finish the UI turn only after AudioService confirms the output path is drained.
    if (turn_complete_pending_.load() && audio_service_.IsPlaybackIdle()) {
        FinishOutputTurn();
    }
    if (!listening_ || !IsAudioChannelOpened()) return;
    std::vector<int16_t> pcm;
    while (audio_service_.PopPcmInput(pcm)) {
        for (size_t i = 0; i < pcm.size(); i += kInputChunkSamples) {
            if (!listening_ || !IsAudioChannelOpened()) return;
            size_t n = std::min(kInputChunkSamples, pcm.size() - i);
            if (!SendAudioChunk(pcm.data() + i, n)) { Fail("Gemini Live could not send microphone audio"); audio_service_.EnablePcmBridge(false); return; }
        }
        pcm.clear();
    }
}
void GeminiLiveProtocol::QueueIncomingFrame(const char* data, size_t size, bool binary) {
    if (data == nullptr || size == 0 || size > kReceiveFrameCapacity || closing_.load()) {
        if (size > kReceiveFrameCapacity) {
            ESP_LOGW(TAG, "Dropping oversized WebSocket frame: %u bytes",
                     static_cast<unsigned>(size));
        }
        return;
    }

    size_t slot = 0;
    portENTER_CRITICAL(&receive_mux_);
    slot = receive_write_index_;
    if (receive_frames_[slot].state != 0) {
        portEXIT_CRITICAL(&receive_mux_);
        const uint32_t dropped = ++receive_frames_dropped_;
        if (dropped == 1 || dropped % 10 == 0) {
            RecordDiagnosticEvent("WebSocket RX ring full; dropped=%lu",
                                  static_cast<unsigned long>(dropped));
        }
        return;
    }
    receive_frames_[slot].state = 1;
    receive_write_index_ = (receive_write_index_ + 1) % kReceiveQueueDepth;
    portEXIT_CRITICAL(&receive_mux_);

    // The WebSocket-owned pointer is valid only during this callback.
    std::memcpy(receive_frames_[slot].data.data(), data, size);
    receive_frames_[slot].size = size;
    receive_frames_[slot].binary = binary;

    portENTER_CRITICAL(&receive_mux_);
    receive_frames_[slot].state = 2;
    portEXIT_CRITICAL(&receive_mux_);

    if (event_group_) xEventGroupSetBits(event_group_, kReceiveFrameAvailable);
    audio_service_.NotifyProtocolPending();
}

void GeminiLiveProtocol::ProcessIncomingFrames() {
    if (event_group_) xEventGroupClearBits(event_group_, kReceiveFrameAvailable);
    while (true) {
        size_t slot = 0;
        size_t size = 0;
        bool binary = false;
        portENTER_CRITICAL(&receive_mux_);
        slot = receive_read_index_;
        if (receive_frames_[slot].state != 2) {
            portEXIT_CRITICAL(&receive_mux_);
            break;
        }
        receive_frames_[slot].state = 1;
        receive_read_index_ = (receive_read_index_ + 1) % kReceiveQueueDepth;
        size = receive_frames_[slot].size;
        binary = receive_frames_[slot].binary;
        portEXIT_CRITICAL(&receive_mux_);

        ProcessIncomingFrame(receive_frames_[slot].data.data(), size, binary);

        portENTER_CRITICAL(&receive_mux_);
        receive_frames_[slot].size = 0;
        receive_frames_[slot].state = 0;
        portEXIT_CRITICAL(&receive_mux_);
    }
}

void GeminiLiveProtocol::ProcessIncomingFrame(const char* data, size_t size, bool binary) {
    if (data == nullptr || size == 0) return;
    // cJSON duplicates strings; do the heap check and all allocations away from
    // the network callback, with a much smaller bounded frame (8 KiB maximum).
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < size * 2 + 10000 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < size) {
        ESP_LOGW(TAG, "Skipping queued Gemini frame because heap is low");
        return;
    }

    cJSON* root = cJSON_ParseWithLength(data, size);
    if (!cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        char prefix[25] = {};
        const size_t preview = std::min<size_t>(size, 8);
        for (size_t i = 0; i < preview; ++i) {
            std::snprintf(prefix + i * 3, sizeof(prefix) - i * 3, "%02X ",
                          static_cast<uint8_t>(data[i]));
        }
        ESP_LOGE(TAG, "Gemini frame is not a JSON object: type=%s size=%u prefix=%s",
                 binary ? "binary" : "text", static_cast<unsigned>(size), prefix);
        Fail(binary ? "Gemini Live sent an unsupported binary WebSocket frame"
                    : "Gemini Live sent invalid JSON in a text frame");
        return;
    }
    if (binary) {
        ESP_LOGW(TAG, "Gemini JSON payload arrived with binary WebSocket opcode (%u bytes)",
                 static_cast<unsigned>(size));
    }
    HandleServerMessage(root);
    cJSON_Delete(root);
    last_incoming_time_ = std::chrono::steady_clock::now();
}

void GeminiLiveProtocol::HandleServerMessage(const cJSON* root) {
    if (cJSON_IsObject(cJSON_GetObjectItem(root, "setupComplete"))) {
        setup_complete_ = true;
        RecordDiagnosticEvent("Gemini setupComplete received");
        xEventGroupSetBits(event_group_, kSetupComplete);
        return;
    }
    const cJSON* error = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsObject(error)) {
        const cJSON* m = cJSON_GetObjectItem(error, "message"); last_server_error_ = cJSON_IsString(m) ? m->valuestring : "Gemini Live server error";
        RecordDiagnosticEvent("Gemini server error received");
        xEventGroupSetBits(event_group_, kSetupFailed); Fail(last_server_error_); return;
    }
    const cJSON* content = cJSON_GetObjectItem(root, "serverContent");
    if (cJSON_IsObject(content)) {
        HandleServerContent(content);
    } else {
        const cJSON* input = cJSON_GetObjectItem(root, "inputTranscription");
        if (cJSON_IsObject(input)) { const cJSON* t = cJSON_GetObjectItem(input, "text"); if (cJSON_IsString(t)) EmitTranscript("stt", nullptr, t->valuestring); }
        const cJSON* output = cJSON_GetObjectItem(root, "outputTranscription");
        if (cJSON_IsObject(output)) { const cJSON* t = cJSON_GetObjectItem(output, "text"); if (cJSON_IsString(t)) { if (!output_turn_started_) { output_turn_started_ = true; EmitTranscript("tts", "start", ""); } EmitTranscript("tts", "sentence_start", t->valuestring); } }
    }
    if (cJSON_IsObject(cJSON_GetObjectItem(root, "goAway"))) ESP_LOGW(TAG, "Gemini Live server requested session shutdown");
}
void GeminiLiveProtocol::HandleServerContent(const cJSON* content) {
    if (cJSON_IsTrue(cJSON_GetObjectItem(content, "interrupted"))) {
        turn_complete_pending_ = false;
        audio_service_.ClearPcmPlayback();
        FinishOutputTurn();
    }
    const cJSON* input = cJSON_GetObjectItem(content, "inputTranscription");
    if (cJSON_IsObject(input)) { const cJSON* t = cJSON_GetObjectItem(input, "text"); if (cJSON_IsString(t)) EmitTranscript("stt", nullptr, t->valuestring); }
    const cJSON* output = cJSON_GetObjectItem(content, "outputTranscription");
    if (cJSON_IsObject(output)) { const cJSON* t = cJSON_GetObjectItem(output, "text"); if (cJSON_IsString(t)) { if (!output_turn_started_) { output_turn_started_ = true; EmitTranscript("tts", "start", ""); } EmitTranscript("tts", "sentence_start", t->valuestring); } }
    const cJSON* turn = cJSON_GetObjectItem(content, "modelTurn"); const cJSON* parts = cJSON_IsObject(turn) ? cJSON_GetObjectItem(turn, "parts") : nullptr;
    if (cJSON_IsArray(parts)) { const cJSON* part = nullptr; cJSON_ArrayForEach(part, parts) HandleAudioPart(part); }
    if (cJSON_IsTrue(cJSON_GetObjectItem(content, "turnComplete"))) {
        turn_complete_pending_ = true;
        if (audio_service_.IsPlaybackIdle()) {
            FinishOutputTurn();
        }
    }
}
void GeminiLiveProtocol::HandleAudioPart(const cJSON* part) {
    const cJSON* in = cJSON_GetObjectItem(part, "inlineData"); if (!cJSON_IsObject(in)) return;
    const cJSON* mime = cJSON_GetObjectItem(in, "mimeType"); const cJSON* data = cJSON_GetObjectItem(in, "data");
    if (!cJSON_IsString(data) || !cJSON_IsString(mime) || std::string(mime->valuestring).find("audio/pcm;rate=24000") == std::string::npos) { ESP_LOGW(TAG, "Ignoring Gemini audio chunk with unsupported MIME type"); return; }
    const size_t encoded_size = std::strlen(data->valuestring);
    if (encoded_size == 0 || encoded_size > kMaxMessage || encoded_size % 4 != 0) {
        ESP_LOGW(TAG, "Ignoring malformed Gemini PCM chunk");
        return;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < 10000 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 8192) {
        ESP_LOGE(TAG, "Low heap, skip");
        return;
    }

    PauseMicrophoneForResponse();
    if (!output_turn_started_) {
        output_turn_started_ = true;
        RecordDiagnosticEvent("Gemini audio playback turn started");
        EmitTranscript("tts", "start", "");
    }
    const bool queued = Decode64InChunks(data->valuestring, encoded_size,
        [this](std::vector<int16_t>&& samples) {
            return audio_service_.PushPcmPlayback(std::move(samples));
        });
    if (!queued) {
        ESP_LOGE(TAG, "Gemini PCM decode or playback enqueue failed");
        Fail("Gemini Live PCM decode or playback failed");
        return;
    }
    const uint32_t received = ++g_response_audio_chunks_received;
    SetPersistedGeminiCounters(g_microphone_chunks_sent.load(), received);
    if (received == 1 || received % 25 == 0) {
        RecordDiagnosticEvent("Gemini response audio chunks=%lu", static_cast<unsigned long>(received));
    }
}
void GeminiLiveProtocol::EmitTranscript(const char* type, const char* state, const char* text) {
    if (!on_incoming_json_) {
        return;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    if (state) {
        cJSON_AddStringToObject(root, "state", state);
    }
    if (text) {
        cJSON_AddStringToObject(root, "text", text);
    }
    on_incoming_json_(root); cJSON_Delete(root);
}
void GeminiLiveProtocol::EmitTtsStop() {
    if (output_turn_started_) {
        EmitTranscript("tts", "stop", "");
        output_turn_started_ = false;
    }
}

void GeminiLiveProtocol::PauseMicrophoneForResponse() {
    if (!listening_) return;
    resume_listening_after_playback_ = true;
    listening_ = false;
    g_microphone_streaming = false;
    SetPersistedGeminiState(g_session_connected.load(), false);
    RecordDiagnosticEvent("Microphone paused for Gemini response");
    audio_service_.EnablePcmBridge(false);
}

void GeminiLiveProtocol::FinishOutputTurn() {
    turn_complete_pending_ = false;
    RecordDiagnosticEvent("Gemini playback turn finished");
    EmitTtsStop();
    if (resume_listening_after_playback_.exchange(false) && IsAudioChannelOpened()) {
        audio_service_.EnablePcmBridge(true);
        listening_ = true;
        g_microphone_streaming = true;
        SetPersistedGeminiState(true, true);
        RecordDiagnosticEvent("Microphone streaming resumed");
    }
}

void GeminiLiveProtocol::Fail(const std::string& message) {
    if (error_occurred_) return;
    error_occurred_ = true;
    listening_ = false;
    turn_complete_pending_ = false;
    resume_listening_after_playback_ = false;
    g_session_connected = false;
    g_microphone_streaming = false;
    std::string safe_message = message;
    if (!api_key_.empty()) {
        size_t pos = 0;
        while ((pos = safe_message.find(api_key_, pos)) != std::string::npos) {
            safe_message.replace(pos, api_key_.size(), "[redacted]");
            pos += 10;
        }
    }
    if (safe_message.size() > 120) safe_message.resize(120);
    { std::lock_guard<std::mutex> lock(g_diagnostics_mutex); g_last_error = safe_message; }
    SetPersistedGeminiState(false, false);
    SetPersistedGeminiError(safe_message.c_str());
    RecordDiagnosticEvent("Gemini failure: %.56s", safe_message.c_str());
    audio_service_.EnablePcmBridge(false);
    audio_service_.EnablePcmPlaybackMode(false);
    audio_service_.ClearPcmPlayback();
    if (!setup_complete_ && event_group_) xEventGroupSetBits(event_group_, kSetupFailed);
    if (on_network_error_) on_network_error_(message);
}
