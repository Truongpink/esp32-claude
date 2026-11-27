#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_err.h>

namespace chatai::display {
struct WebSnapshot {
    bool oled_ready = false;
    char clock[9] = "--:--:--";
    char chat_message[96]{};
    char footer[96]{};
};
void GetWebSnapshot(WebSnapshot& snapshot);
esp_err_t Initialize();
void SetAudioReady(bool ready);
void SetChatMessage(const char* role, const char* message);
void ClearChatMessages();
enum class BluetoothStatus : uint8_t {
    kIdle,
    kInitializing,
    kReady,
    kScanning,
    kComplete,
    kError
};
void SetBluetoothStatus(BluetoothStatus status, size_t device_count = 0,
                        int error_code = 0);
void Notify(const char* title, const char* message, uint32_t duration_ms = 3000);
}  // namespace chatai::display
