#pragma once
#include <cstdint>
#include <esp_err.h>
namespace chatai::xiaozhi {
enum class RuntimeState : uint8_t { kConnecting, kWaiting, kListening, kProcessing, kSpeaking, kError };
enum class BootAction : uint8_t { kStarted, kStopped, kAborted, kIgnored, kFailed };
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
}  // namespace chatai::xiaozhi
