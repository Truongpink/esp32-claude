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
