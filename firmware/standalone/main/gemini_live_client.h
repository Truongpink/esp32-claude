#pragma once

#include <esp_err.h>

namespace chatai::gemini {

enum class NotificationSound : unsigned char { kSuccess = 1, kFirmwareUpdated = 2, kWarning = 3, kRollback = 4 };
void QueueNotificationSound(NotificationSound sound);
esp_err_t Initialize();
bool IsReady();
bool IsSpeaking();
void AbortSpeaking();
bool HasError();
bool IsTlsFatal();
void SetChatEnabled(bool enabled);
bool IsChatEnabled();
void SetPushToTalk(bool pressed);
bool IsListening();
void RequestReconnect();

}  // namespace chatai::gemini
