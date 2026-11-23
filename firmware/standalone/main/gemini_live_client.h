#pragma once

// Gemini Live is intentionally compiled out for the current hardware-isolation
// phase. No-op compatibility API keeps legacy integrations link-safe without
// starting a Gemini worker, WebSocket, reconnect loop, or audio task.
#include <esp_err.h>

namespace chatai::gemini {
enum class NotificationSound : unsigned char {
    kSuccess = 1, kFirmwareUpdated = 2, kWarning = 3, kRollback = 4
};
inline constexpr bool kEnabled = false;
inline void QueueNotificationSound(NotificationSound) {}
inline esp_err_t Initialize() { return ESP_OK; }
inline bool IsReady() { return false; }
inline bool IsSpeaking() { return false; }
inline void AbortSpeaking() {}
inline bool HasError() { return false; }
inline bool IsTlsFatal() { return false; }
inline void SetChatEnabled(bool) {}
inline bool IsChatEnabled() { return false; }
inline void SetPushToTalk(bool) {}
inline bool IsListening() { return false; }
inline void RequestReconnect() {}
}  // namespace chatai::gemini
