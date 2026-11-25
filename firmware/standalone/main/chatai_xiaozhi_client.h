#pragma once
#include <cstdint>
#include <esp_err.h>
namespace chatai::xiaozhi {
enum class RuntimeState : uint8_t { kConnecting, kWaiting, kListening, kProcessing, kSpeaking, kError };
esp_err_t Initialize();
bool IsReady();
bool StartConversation();
void SetChatEnabled(bool enabled);
bool IsChatEnabled();
void SetPushToTalk(bool pressed);
bool IsListening();
RuntimeState GetRuntimeState();
}  // namespace chatai::xiaozhi
