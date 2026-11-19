#pragma once

#include <esp_err.h>

namespace chatai::gemini {

esp_err_t Initialize();
bool IsReady();
bool IsSpeaking();
bool HasError();
bool IsTlsFatal();
void SetChatEnabled(bool enabled);
bool IsChatEnabled();
void SetPushToTalk(bool pressed);
bool IsListening();
void RequestReconnect();

}  // namespace chatai::gemini
