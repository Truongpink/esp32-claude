#ifndef CHATAI_GEMINI_LIVE_PROTOCOL_H
#define CHATAI_GEMINI_LIVE_PROTOCOL_H
#include "protocol.h"
#include "audio_service.h"
#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <atomic>
#include <array>
#include <memory>
#include <cstdint>
#include <string>
class GeminiLiveProtocol final : public Protocol {
public:
    struct Diagnostics {
        uint32_t microphone_chunks_sent = 0;
        uint32_t response_audio_chunks_received = 0;
        bool session_connected = false;
        bool microphone_streaming = false;
        std::string last_error;
        std::string event_history;
        std::string previous_session_summary;
    };
    static void InitializeDiagnosticHistory();
    static Diagnostics GetDiagnostics();
    explicit GeminiLiveProtocol(AudioService& audio_service);
    ~GeminiLiveProtocol() override;
    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool SendLiveVoiceNotification(const std::string& text) override;
    void ProcessPending() override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendWakeWordDetected(const std::string& wake_word) override;
private:
    static constexpr EventBits_t kSetupComplete = BIT0;
    static constexpr EventBits_t kSetupFailed = BIT1;
    static constexpr EventBits_t kReceiveFrameAvailable = BIT2;
    static constexpr size_t kReceiveFrameCapacity = 8192;
    static constexpr size_t kReceiveQueueDepth = 3;
    static constexpr size_t kInputChunkSamples = 320;
    struct ReceiveFrameSlot {
        std::array<char, kReceiveFrameCapacity> data{};
        size_t size = 0;
        bool binary = false;
        uint8_t state = 0; // 0=free, 1=reserved/processing, 2=ready
    };
    AudioService& audio_service_;
    std::array<ReceiveFrameSlot, kReceiveQueueDepth> receive_frames_{};
    size_t receive_write_index_ = 0;
    size_t receive_read_index_ = 0;
    portMUX_TYPE receive_mux_ = portMUX_INITIALIZER_UNLOCKED;
    std::atomic<uint32_t> receive_frames_dropped_{0};
    std::shared_ptr<WebSocket> websocket_;
    EventGroupHandle_t event_group_ = nullptr;
    std::atomic<bool> setup_complete_{false}, listening_{false}, closing_{false};
    std::atomic<bool> channel_open_notified_{false};
    std::atomic<bool> output_turn_started_{false};
    std::atomic<bool> turn_complete_pending_{false};
    std::atomic<bool> resume_listening_after_playback_{false};
    std::string api_key_, model_, bot_name_, voice_name_, language_code_, system_instruction_, last_server_error_;
    bool SendText(const std::string& text) override;
    bool SendSetup();
    bool SendAudioChunk(const int16_t* samples, size_t count);
    bool SendAudioStreamEnd();
    void QueueIncomingFrame(const char* data, size_t size, bool binary);
    void ProcessIncomingFrames();
    void ProcessIncomingFrame(const char* data, size_t size, bool binary);
    void HandleServerMessage(const cJSON* root);
    void HandleServerContent(const cJSON* content);
    void HandleAudioPart(const cJSON* part);
    void EmitTranscript(const char* type, const char* state, const char* text);
    void EmitTtsStop();
    void PauseMicrophoneForResponse();
    void FinishOutputTurn();
    void Fail(const std::string& message);
};
#endif
