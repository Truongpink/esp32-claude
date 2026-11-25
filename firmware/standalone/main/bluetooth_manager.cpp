#include "bluetooth_manager.h"

namespace chatai::bluetooth {

esp_err_t StartScan() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t StopScan() { return ESP_OK; }
bool IsInitialized() { return false; }
bool IsReady() { return false; }
bool IsScanning() { return false; }
bool IsScanRequested() { return false; }
size_t GetDeviceCount() { return 0; }
size_t GetDevices(Device* output, size_t capacity) {
    (void)output;
    (void)capacity;
    return 0;
}
int GetLastError() { return ESP_ERR_NOT_SUPPORTED; }

}  // namespace chatai::bluetooth
