#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_err.h>

namespace chatai::display {
esp_err_t Initialize();
void SetAudioReady(bool ready);
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
