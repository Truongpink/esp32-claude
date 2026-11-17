#include "gemini_live_protocol.h"
#include "board.h"
#include "settings.h"
#include <esp_log.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <mutex>
#define TAG "GeminiLive"
namespace {
constexpr char kUrl[] = "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent";
constexpr size_t kMaxMessage = 64 * 1024;
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::atomic<uint32_t> g_microphone_chunks_sent{0};
std::atomic<uint32_t> g_response_audio_chunks_received{0};
std::atomic<bool> g_session_connected{false};
std::atomic<bool> g_microphone_streaming{false};
std::mutex g_diagnostics_mutex;
std::string g_last_error;
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
bool Decode64(const char* data, size_t size, std::vector<uint8_t>& out) {
    if (!data || !size || size > kMaxMessage || size % 4 != 0) return false;
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear(); out.reserve((size / 4) * 3);
    for (size_t i = 0; i < size; i += 4) {
        const bool pad2 = data[i + 2] == '=', pad3 = data[i + 3] == '=';
        if ((pad2 && !pad3) || ((pad2 || pad3) && i + 4 != size)) return false;
        const int a = value(data[i]), b = value(data[i + 1]);
        const int c = pad2 ? 0 : value(data[i + 2]), d = pad3 ? 0 : value(data[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        const uint32_t v = (static_cast<uint32_t>(a) << 18) | (static_cast<uint32_t>(b) << 12) |
                           (static_cast<uint32_t>(c) << 6) | static_cast<uint32_t>(d);
        out.push_back(static_cast<uint8_t>(v >> 16));
        if (!pad2) out.push_back(static_cast<uint8_t>(v >> 8));
        if (!pad3) out.push_back(static_cast<uint8_t>(v));
    }
    return true;
}
std::string PrintJson(cJSON* root) {
    char* p = cJSON_PrintUnformatted(root); if (!p) return {};
    std::string s(p); cJSON_free(p); return s;
}
}
GeminiLiveProtocol::GeminiLiveProtocol(AudioService& audio) : audio_service_(audio) { event_group_ = xEventGroupCreate(); }
GeminiLiveProtocol::~GeminiLiveProtocol() { CloseAudioChannel(false); if (event_group_) vEventGroupDelete(event_group_); }
GeminiLiveProtocol::Diagnostics GeminiLiveProtocol::GetDiagnostics() {
    Diagnostics result;
    result.microphone_chunks_sent = g_microphone_chunks_sent.load();
    result.response_audio_chunks_received = g_response_audio_chunks_received.load();
    result.session_connected = g_session_connected.load();
    result.microphone_streaming = g_microphone_streaming.load();
    std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
    result.last_error = g_last_error;
    return result;
}
bool GeminiLiveProtocol::Start() { return true; }
bool GeminiLiveProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) { (void)packet; return false; }
bool GeminiLiveProtocol::SendLiveVoiceNotification(const std::string& text) {
    if (text.empty() || !listening_ || output_turn_started_.load() || !IsAudioChannelOpened()) {
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
    error_occurred_ = false; closing_ = false; listening_ = false; output_turn_started_ = false; setup_complete_ = false; last_server_error_.clear();
    xEventGroupClearBits(event_group_, kSetupComplete | kSetupFailed);
    auto* network = Board::GetInstance().GetNetwork();
    if (!network) { Fail("Gemini Live network interface is unavailable"); return false; }
    auto socket = network->CreateWebSocket(1);
    if (!socket) { Fail("Failed to create Gemini Live WebSocket"); return false; }
    websocket_ = std::shared_ptr<WebSocket>(std::move(socket));
    websocket_->OnData([this](const char* data, size_t size, bool binary) {
        if (data == nullptr || size == 0) {
            ESP_LOGE(TAG, "Gemini Live received an empty WebSocket frame (binary=%d)", binary);
            Fail("Gemini Live sent an empty WebSocket frame");
            return;
        }
        if (size > kMaxMessage) {
            ESP_LOGE(TAG, "Gemini Live frame exceeds limit: %u bytes (binary=%d)",
                     static_cast<unsigned>(size), binary);
            Fail("Gemini Live WebSocket frame is too large");
            return;
        }

        // Gemini Live server messages are JSON envelopes. Parse the payload even
        // when the transport marks it binary, so a wrapper opcode mismatch is
        // distinguishable from genuinely non-JSON binary data.
        cJSON* root = cJSON_ParseWithLength(data, size);
        if (!cJSON_IsObject(root)) {
            if (root) cJSON_Delete(root);
            char prefix[25] = {};
            const size_t preview = std::min<size_t>(size, 8);
            for (size_t i = 0; i < preview; ++i) {
                std::snprintf(prefix + i * 3, sizeof(prefix) - i * 3, "%02X ", static_cast<uint8_t>(data[i]));
            }
            ESP_LOGE(TAG, "Gemini Live frame is not a JSON object: type=%s size=%u prefix=%s",
                     binary ? "binary" : "text", static_cast<unsigned>(size), prefix);
            Fail(binary ? "Gemini Live sent an unsupported binary WebSocket frame"
                        : "Gemini Live sent invalid JSON in a text frame");
            return;
        }
        if (binary) {
            ESP_LOGW(TAG, "Gemini Live JSON payload arrived with binary WebSocket opcode (%u bytes)",
                     static_cast<unsigned>(size));
        }
        HandleServerMessage(root);
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });
    websocket_->OnError([this](const NetworkError&) { if (!closing_) { if (!setup_complete_) xEventGroupSetBits(event_group_, kSetupFailed); Fail("Gemini Live WebSocket transport error"); } });
    websocket_->OnDisconnected([this]() {
        if (!setup_complete_) xEventGroupSetBits(event_group_, kSetupFailed);
        setup_complete_ = false; listening_ = false; g_session_connected = false; g_microphone_streaming = false; audio_service_.EnablePcmBridge(false); audio_service_.EnablePcmPlaybackMode(false); audio_service_.ClearPcmPlayback();
        if (!closing_ && channel_open_notified_.exchange(false) && on_audio_channel_closed_) on_audio_channel_closed_();
    });
    // The Gemini Live WebSocket endpoint authenticates with the API key in the query string.
    // Keep the key out of logs; do not log this URL.
    const std::string url = std::string(kUrl) + "?key=" + api_key_;
    if (auto connected = websocket_->Connect(url.c_str()); !connected) { Fail("Could not connect to Gemini Live"); CloseAudioChannel(false); return false; }
    if (!SendSetup()) { Fail("Could not send Gemini Live setup"); CloseAudioChannel(false); return false; }
    EventBits_t bits = xEventGroupWaitBits(event_group_, kSetupComplete | kSetupFailed, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
    if (!(bits & kSetupComplete)) { Fail(last_server_error_.empty() ? "Gemini Live setup timed out" : last_server_error_); CloseAudioChannel(false); return false; }
    setup_complete_ = true; g_session_connected = true; audio_service_.EnablePcmPlaybackMode(true); last_incoming_time_ = std::chrono::steady_clock::now();
    channel_open_notified_ = true; if (on_connected_) on_connected_(); if (on_audio_channel_opened_) on_audio_channel_opened_();
    ESP_LOGI(TAG, "Gemini Live session ready; model=%s", model_.c_str()); return true;
}
void GeminiLiveProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye; closing_ = true; listening_ = false; setup_complete_ = false; g_session_connected = false; g_microphone_streaming = false;
    audio_service_.EnablePcmBridge(false); audio_service_.EnablePcmPlaybackMode(false); audio_service_.ClearPcmPlayback();
    if (channel_open_notified_.exchange(false) && on_audio_channel_closed_) on_audio_channel_closed_();
    if (websocket_) { websocket_->Close(); websocket_.reset(); } api_key_.clear();
}
void GeminiLiveProtocol::SendStartListening(ListeningMode mode) { (void)mode; if (IsAudioChannelOpened()) { audio_service_.EnablePcmBridge(true); listening_ = true; g_microphone_streaming = true; } }
void GeminiLiveProtocol::SendStopListening() {
    if (!IsAudioChannelOpened()) { listening_ = false; return; }
    ProcessPending(); listening_ = false; g_microphone_streaming = false; audio_service_.EnablePcmBridge(false); SendAudioStreamEnd();
}
void GeminiLiveProtocol::SendAbortSpeaking(AbortReason reason) { (void)reason; audio_service_.ClearPcmPlayback(); }
void GeminiLiveProtocol::SendWakeWordDetected(const std::string& word) { (void)word; }
bool GeminiLiveProtocol::SendAudioChunk(const int16_t* samples, size_t count) {
    if (!samples || !count || !IsAudioChannelOpened()) return false;
    std::string b64 = Encode64(reinterpret_cast<const uint8_t*>(samples), count * sizeof(int16_t)); if (b64.empty()) return false;
    cJSON* root = cJSON_CreateObject(); cJSON* realtime = cJSON_AddObjectToObject(root, "realtimeInput");
    cJSON* audio = cJSON_AddObjectToObject(realtime, "audio"); cJSON_AddStringToObject(audio, "mimeType", "audio/pcm;rate=16000");
    cJSON_AddStringToObject(audio, "data", b64.c_str()); std::string msg = PrintJson(root); cJSON_Delete(root);
    if (msg.empty() || !websocket_->Send(msg)) return false;
    ++g_microphone_chunks_sent;
    return true;
}
bool GeminiLiveProtocol::SendAudioStreamEnd() {
    cJSON* root = cJSON_CreateObject(); cJSON* realtime = cJSON_AddObjectToObject(root, "realtimeInput"); cJSON_AddBoolToObject(realtime, "audioStreamEnd", true);
    std::string msg = PrintJson(root); cJSON_Delete(root); return !msg.empty() && SendText(msg);
}
void GeminiLiveProtocol::ProcessPending() {
    if (!listening_ || !IsAudioChannelOpened()) return;
    std::vector<int16_t> pcm;
    while (audio_service_.PopPcmInput(pcm)) {
        for (size_t i = 0; i < pcm.size(); i += kInputChunkSamples) {
            size_t n = std::min(kInputChunkSamples, pcm.size() - i);
            if (!SendAudioChunk(pcm.data() + i, n)) { Fail("Gemini Live could not send microphone audio"); audio_service_.EnablePcmBridge(false); return; }
        }
        pcm.clear();
    }
}
void GeminiLiveProtocol::HandleServerMessage(const cJSON* root) {
    if (cJSON_IsObject(cJSON_GetObjectItem(root, "setupComplete"))) { xEventGroupSetBits(event_group_, kSetupComplete); return; }
    const cJSON* error = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsObject(error)) {
        const cJSON* m = cJSON_GetObjectItem(error, "message"); last_server_error_ = cJSON_IsString(m) ? m->valuestring : "Gemini Live server error";
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
    if (cJSON_IsTrue(cJSON_GetObjectItem(content, "interrupted"))) { audio_service_.ClearPcmPlayback(); EmitTtsStop(); }
    const cJSON* input = cJSON_GetObjectItem(content, "inputTranscription");
    if (cJSON_IsObject(input)) { const cJSON* t = cJSON_GetObjectItem(input, "text"); if (cJSON_IsString(t)) EmitTranscript("stt", nullptr, t->valuestring); }
    const cJSON* output = cJSON_GetObjectItem(content, "outputTranscription");
    if (cJSON_IsObject(output)) { const cJSON* t = cJSON_GetObjectItem(output, "text"); if (cJSON_IsString(t)) { if (!output_turn_started_) { output_turn_started_ = true; EmitTranscript("tts", "start", ""); } EmitTranscript("tts", "sentence_start", t->valuestring); } }
    const cJSON* turn = cJSON_GetObjectItem(content, "modelTurn"); const cJSON* parts = cJSON_IsObject(turn) ? cJSON_GetObjectItem(turn, "parts") : nullptr;
    if (cJSON_IsArray(parts)) { const cJSON* part = nullptr; cJSON_ArrayForEach(part, parts) HandleAudioPart(part); }
    if (cJSON_IsTrue(cJSON_GetObjectItem(content, "turnComplete"))) EmitTtsStop();
}
void GeminiLiveProtocol::HandleAudioPart(const cJSON* part) {
    const cJSON* in = cJSON_GetObjectItem(part, "inlineData"); if (!cJSON_IsObject(in)) return;
    const cJSON* mime = cJSON_GetObjectItem(in, "mimeType"); const cJSON* data = cJSON_GetObjectItem(in, "data");
    if (!cJSON_IsString(data) || !cJSON_IsString(mime) || std::string(mime->valuestring).find("audio/pcm;rate=24000") == std::string::npos) { ESP_LOGW(TAG, "Ignoring Gemini audio chunk with unsupported MIME type"); return; }
    std::vector<uint8_t> decoded;
    if (!Decode64(data->valuestring, std::strlen(data->valuestring), decoded) || decoded.size() < 2 || decoded.size() % 2) { ESP_LOGW(TAG, "Ignoring malformed Gemini PCM chunk"); return; }
    if (!output_turn_started_) { output_turn_started_ = true; EmitTranscript("tts", "start", ""); }
    ++g_response_audio_chunks_received;
    std::vector<int16_t> samples(decoded.size() / 2); std::memcpy(samples.data(), decoded.data(), decoded.size());
    for (size_t i = 0; i < samples.size(); i += kOutputChunkSamples) {
        size_t n = std::min(kOutputChunkSamples, samples.size() - i);
        audio_service_.PushPcmPlayback(std::vector<int16_t>(samples.begin() + i, samples.begin() + i + n));
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
    // Keep realtime microphone capture active between Gemini turns.
    // The application returns from Speaking to Listening after this event;
    // disabling the PCM bridge here would leave that next turn without audio.
}
void GeminiLiveProtocol::Fail(const std::string& message) {
    if (error_occurred_) return;
    error_occurred_ = true;
    listening_ = false;
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
    audio_service_.EnablePcmBridge(false);
    audio_service_.EnablePcmPlaybackMode(false);
    audio_service_.ClearPcmPlayback();
    if (!setup_complete_ && event_group_) xEventGroupSetBits(event_group_, kSetupFailed);
    if (on_network_error_) on_network_error_(message);
}
