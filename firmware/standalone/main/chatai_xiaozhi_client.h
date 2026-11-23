#pragma once
#include <esp_err.h>
namespace chatai::xiaozhi {
esp_err_t Initialize();
bool IsReady();
bool StartConversation();
void SetChatEnabled(bool enabled);
bool IsChatEnabled();
void SetPushToTalk(bool pressed);
bool IsListening();
}  // namespace chatai::xiaozhi
