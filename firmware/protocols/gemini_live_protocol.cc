#include "gemini_live_protocol.h"
#include "board.h"
#include "settings.h"
#include <mbedtls/base64.h>
#include <esp_log.h>
#include <algorithm>
#include <cstring>
#define TAG "GeminiLive"
namespace {
constexpr char kUrl[] = "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent";
constexpr size_t kMaxMessage = 64 * 1024;
std::string Encode64(const uint8_t* p, size_t n) {
    std::string out(4 * ((n + 2) / 3) + 1, '\0'); size_t written = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &written, p, n) != 0) return {};
    out.resize(written); return out;
}
bool Decode64(const char* p, size_t n, std::vector<uint8_t>& out) {
    if (!p || !n || n > kMaxMessage) return false;
    out.resize((n * 3) / 4 + 3); size_t written = 0;
    if (mbedtls_base64_decode(out.data(), out.size(), &written, reinterpret_cast<const unsigned char*>(p), n) != 0) { out.clear(); return false; }
    out.resize(written); return true;
}
std::string PrintJson(cJSON* root) {
    char* p = cJSON_PrintUnformatted(root); if (!p) return {};
    std::string s(p); cJSON_free(p); return s;
}
}
GeminiLiveProtocol::GeminiLiveProtocol(AudioService& audio) : audio_service_(audio) { event_group_ = xEventGroupCreate(); }
GeminiLiveProtocol::~GeminiLiveProtocol() { CloseAudioChannel(false); if (event_group_) vEventGroupDelete(event_group_); }
bool GeminiLiveProtocol::Start() { return true; }
bool GeminiLiveProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) { (void)packet; return false; }
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
    cJSON* voice = cJSON_AddObjectToObject(speech, "voiceConfig");
    cJSON* prebuilt = cJSON_AddObjectToObject(voice, "prebuiltVoiceConfig"); cJSON_AddStringToObject(prebuilt, "voiceName", "Puck");
    cJSON* instruction = cJSON_AddObjectToObject(setup, "systemInstruction");
    cJSON* parts = cJSON_AddArrayToObject(instruction, "parts"); cJSON* part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", system_instruction_.c_str()); cJSON_AddItemToArray(parts, part);
    cJSON_AddObjectToObject(setup, "inputAudioTranscription"); cJSON_AddObjectToObject(setup, "outputAudioTranscription");
    std::string msg = PrintJson(root); cJSON_Delete(root); return !msg.empty() && websocket_->Send(msg);
}
bool GeminiLiveProtocol::OpenAudioChannel() {
    Settings cfg("gemini", false);
    api_key_ = cfg.GetString("api_key"); model_ = cfg.GetString("model", "gemini-3.8-live");
    system_instruction_ = cfg.GetString("system_instruction", "You are ChatAI TPIOS, a helpful Vietnamese voice assistant. Speak naturally in Vietnamese unless the user asks for another language. Keep replies concise and conversational.");
    if (api_key_.empty()) { Fail("Gemini API key is not configured in local device settings"); return false; }
    if (model_.rfind("models/", 0) != 0) model_ = "models/" + model_;
    error_occurred_ = false; closing_ = false; listening_ = false; output_turn_started_ = false; setup_complete_ = false; last_server_error_.clear();
    xEventGroupClearBits(event_group_, kSetupComplete | kSetupFailed);
    auto* network = Board::GetInstance().GetNetwork();
    if (!network) { Fail("Gemini Live network interface is unavailable"); return false; }
    auto socket = network->CreateWebSocket(1);
    if (!socket) { Fail("Failed to create Gemini Live WebSocket"); return false; }
    websocket_ = std::shared_ptr<WebSocket>(std::move(socket));
    websocket_->SetHeader("x-goog-api-key", api_key_.c_str());
    websocket_->OnData([this](const char* data, size_t size, bool binary) {
        if (binary || !size || size > kMaxMessage) { Fail("Invalid Gemini Live WebSocket message"); return; }
        cJSON* root = cJSON_ParseWithLength(data, size);
        if (!root) { Fail("Gemini Live returned invalid JSON"); return; }
        HandleServerMessage(root); cJSON_Delete(root); last_incoming_time_ = std::chrono::steady_clock::now();
    });
    websocket_->OnError([this](const NetworkError&) { if (!closing_) { if (!setup_complete_) xEventGroupSetBits(event_group_, kSetupFailed); Fail("Gemini Live WebSocket transport error"); } });
    websocket_->OnDisconnected([this]() {
        if (!setup_complete_) xEventGroupSetBits(event_group_, kSetupFailed);
        setup_complete_ = false; listening_ = false; audio_service_.EnablePcmBridge(false); audio_service_.ClearPcmPlayback();
        if (!closing_ && channel_open_notified_.exchange(false) && on_audio_channel_closed_) on_audio_channel_closed_();
    });
    if (auto connected = websocket_->Connect(kUrl); !connected) { Fail("Could not connect to Gemini Live"); CloseAudioChannel(false); return false; }
    if (!SendSetup()) { Fail("Could not send Gemini Live setup"); CloseAudioChannel(false); return false; }
    EventBits_t bits = xEventGroupWaitBits(event_group_, kSetupComplete | kSetupFailed, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
    if (!(bits & kSetupComplete)) { Fail(last_server_error_.empty() ? "Gemini Live setup timed out" : last_server_error_); CloseAudioChannel(false); return false; }
    setup_complete_ = true; audio_service_.EnablePcmBridge(true); last_incoming_time_ = std::chrono::steady_clock::now();
    channel_open_notified_ = true; if (on_connected_) on_connected_(); if (on_audio_channel_opened_) on_audio_channel_opened_();
    ESP_LOGI(TAG, "Gemini Live session ready; model=%s", model_.c_str()); return true;
}
void GeminiLiveProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye; closing_ = true; listening_ = false; setup_complete_ = false;
    audio_service_.EnablePcmBridge(false); audio_service_.ClearPcmPlayback();
    if (channel_open_notified_.exchange(false) && on_audio_channel_closed_) on_audio_channel_closed_();
    if (websocket_) { websocket_->Close(); websocket_.reset(); } api_key_.clear();
}
void GeminiLiveProtocol::SendStartListening(ListeningMode mode) { (void)mode; if (IsAudioChannelOpened()) listening_ = true; }
void GeminiLiveProtocol::SendStopListening() {
    if (!IsAudioChannelOpened()) { listening_ = false; return; }
    ProcessPending(); listening_ = false; SendAudioStreamEnd();
}
void GeminiLiveProtocol::SendAbortSpeaking(AbortReason reason) { (void)reason; audio_service_.ClearPcmPlayback(); }
void GeminiLiveProtocol::SendWakeWordDetected(const std::string& word) { (void)word; }
bool GeminiLiveProtocol::SendAudioChunk(const int16_t* samples, size_t count) {
    if (!samples || !count || !IsAudioChannelOpened()) return false;
    std::string b64 = Encode64(reinterpret_cast<const uint8_t*>(samples), count * sizeof(int16_t)); if (b64.empty()) return false;
    cJSON* root = cJSON_CreateObject(); cJSON* realtime = cJSON_AddObjectToObject(root, "realtimeInput");
    cJSON* audio = cJSON_AddObjectToObject(realtime, "audio"); cJSON_AddStringToObject(audio, "mimeType", "audio/pcm;rate=16000");
    cJSON_AddStringToObject(audio, "data", b64.c_str()); std::string msg = PrintJson(root); cJSON_Delete(root);
    return !msg.empty() && websocket_->Send(msg);
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
    const cJSON* content = cJSON_GetObjectItem(root, "serverContent"); if (cJSON_IsObject(content)) HandleServerContent(content);
    const cJSON* input = cJSON_GetObjectItem(root, "inputTranscription");
    if (cJSON_IsObject(input)) { const cJSON* t = cJSON_GetObjectItem(input, "text"); if (cJSON_IsString(t)) EmitTranscript("stt", nullptr, t->valuestring); }
    const cJSON* output = cJSON_GetObjectItem(root, "outputTranscription");
    if (cJSON_IsObject(output)) { const cJSON* t = cJSON_GetObjectItem(output, "text"); if (cJSON_IsString(t)) { if (!output_turn_started_) { output_turn_started_ = true; EmitTranscript("tts", "start", ""); } EmitTranscript("tts", "sentence_start", t->valuestring); } }
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
    std::vector<int16_t> samples(decoded.size() / 2); std::memcpy(samples.data(), decoded.data(), decoded.size());
    for (size_t i = 0; i < samples.size(); i += kOutputChunkSamples) {
        size_t n = std::min(kOutputChunkSamples, samples.size() - i);
        audio_service_.PushPcmPlayback(std::vector<int16_t>(samples.begin() + i, samples.begin() + i + n));
    }
}
void GeminiLiveProtocol::EmitTranscript(const char* type, const char* state, const char* text) {
    if (!on_incoming_json_) return; cJSON* root = cJSON_CreateObject(); cJSON_AddStringToObject(root, "type", type);
    if (state) cJSON_AddStringToObject(root, "state", state); if (text) cJSON_AddStringToObject(root, "text", text);
    on_incoming_json_(root); cJSON_Delete(root);
}
void GeminiLiveProtocol::EmitTtsStop() { if (output_turn_started_) { EmitTranscript("tts", "stop", ""); output_turn_started_ = false; } }
void GeminiLiveProtocol::Fail(const std::string& message) {
    if (error_occurred_) return; error_occurred_ = true; if (on_network_error_) on_network_error_(message);
}
