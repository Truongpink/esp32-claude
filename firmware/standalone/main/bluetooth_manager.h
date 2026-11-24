#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>

namespace chatai::bluetooth {

constexpr size_t kMaxDevices = 32;

struct Device {
    char name[32]{};
    char address[18]{};
    char address_type[12]{};
    char manufacturer_data[49]{};
    char service_uuids[128]{};
    char advertising_data[64]{};
    int8_t rssi = -127;
    uint32_t last_seen_seconds = 0;
    uint64_t last_seen_us = 0;
};

esp_err_t StartScan();
esp_err_t StopScan();
bool IsInitialized();
bool IsReady();
bool IsScanning();
bool IsScanRequested();
size_t GetDeviceCount();
size_t GetDevices(Device* output, size_t capacity);
int GetLastError();

}  // namespace chatai::bluetooth
