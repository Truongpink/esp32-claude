#pragma once
#include <cstdint>
#include <esp_err.h>
namespace chatai::xiaozhi {
enum class RuntimeState : uint8_t { kConnecting, kWaiting, kListening, kProcessing, kSpeaking, kError };
enum class BootAction : uint8_t { kStarted, kStopped, kAborted, kIgnored, kFailed };
struct AudioUplinkDiagnostics {
    bool available = false;
    uint32_t pcm_frames_queued = 0;
    uint32_t opus_frames_encoded = 0;
    uint32_t encode_failures = 0;
    uint32_t encode_queue_drops = 0;
    uint32_t send_queue_drops = 0;
};
esp_err_t Initialize();
void Run();
bool IsReady();
bool StartConversation();
void SetChatEnabled(bool enabled);
bool IsChatEnabled();
void SetPushToTalk(bool pressed);
bool IsListening();
RuntimeState GetRuntimeState();
BootAction HandleBootToggle();
AudioUplinkDiagnostics GetAudioUplinkDiagnostics();
}  // namespace chatai::xiaozhi
