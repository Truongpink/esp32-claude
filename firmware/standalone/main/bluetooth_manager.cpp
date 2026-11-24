#include "bluetooth_manager.h"
#include "display_manager.h"
#include "diagnostic_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/ble_hs_adv.h>

namespace chatai::bluetooth {
namespace {
constexpr char kTag[] = "ChatAI-BLE";
constexpr int32_t kScanDurationMs = 10000;
constexpr size_t kManufacturerBytes = 16;
constexpr size_t kAdvertisingBytes = 31;
constexpr uint32_t kInitTaskStackBytes = 6144;
constexpr UBaseType_t kInitTaskPriority = 5;

std::array<Device, kMaxDevices> g_devices{};
size_t g_device_count = 0;
std::mutex g_devices_mutex;
std::mutex g_init_mutex;
std::mutex g_scan_mutex;
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_initializing{false};
std::atomic<bool> g_host_synced{false};
std::atomic<bool> g_scan_requested{false};
std::atomic<bool> g_scanning{false};
std::atomic<int> g_last_error{0};

void CopyText(char* destination, size_t capacity, const char* source, size_t length) {
    if (destination == nullptr || capacity == 0) return;
    const size_t count = std::min(length, capacity - 1);
    if (source != nullptr && count > 0) std::memcpy(destination, source, count);
    destination[count] = '\0';
}

void FormatHex(const uint8_t* data, size_t length, char* output, size_t capacity,
               size_t max_bytes) {
    if (output == nullptr || capacity == 0) return;
    output[0] = '\0';
    if (data == nullptr) return;
    const size_t count = std::min({length, max_bytes, (capacity - 1) / 2});
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = i * 2;
        std::snprintf(output + offset, capacity - offset, "%02X", data[i]);
    }
}

void AppendUuid(char* output, size_t capacity, const char* value) {
    const size_t used = std::strlen(output);
    if (used >= capacity - 1) return;
    if (used > 0) {
        output[used] = ',';
        output[used + 1] = '\0';
    }
    const size_t next = std::strlen(output);
    if (next < capacity - 1) {
        std::strncat(output, value, capacity - next - 1);
    }
}

void BuildServiceUuids(const ble_hs_adv_fields& fields, char* output, size_t capacity) {
    if (capacity == 0) return;
    output[0] = '\0';
    char value[40]{};
    for (uint8_t i = 0; i < fields.num_uuids16; ++i) {
        std::snprintf(value, sizeof(value), "16:%04X", fields.uuids16[i].value);
        AppendUuid(output, capacity, value);
    }
    for (uint8_t i = 0; i < fields.num_uuids32; ++i) {
        std::snprintf(value, sizeof(value), "32:%08lX",
                      static_cast<unsigned long>(fields.uuids32[i].value));
        AppendUuid(output, capacity, value);
    }
    for (uint8_t i = 0; i < fields.num_uuids128; ++i) {
        char uuid[33]{};
        for (int byte = 15; byte >= 0; --byte) {
            const size_t offset = static_cast<size_t>(15 - byte) * 2;
            std::snprintf(uuid + offset, sizeof(uuid) - offset, "%02X",
                          fields.uuids128[i].value[byte]);
        }
        std::snprintf(value, sizeof(value), "128:%s", uuid);
        AppendUuid(output, capacity, value);
    }
}

const char* AddressTypeName(uint8_t type) {
    switch (type) {
        case BLE_ADDR_PUBLIC: return "Public";
        case BLE_ADDR_RANDOM: return "Random";
        case BLE_ADDR_PUBLIC_ID: return "Public ID";
        case BLE_ADDR_RANDOM_ID: return "Random ID";
        default: return "Unknown";
    }
}

void UpdateDevice(const ble_gap_disc_desc& report,
                  const ble_hs_adv_fields* fields) {
    Device incoming{};
    std::snprintf(incoming.address, sizeof(incoming.address),
                  "%02X:%02X:%02X:%02X:%02X:%02X",
                  report.addr.val[5], report.addr.val[4], report.addr.val[3],
                  report.addr.val[2], report.addr.val[1], report.addr.val[0]);
    CopyText(incoming.address_type, sizeof(incoming.address_type),
             AddressTypeName(report.addr.type),
             std::strlen(AddressTypeName(report.addr.type)));
    incoming.rssi = report.rssi;
    incoming.last_seen_us = static_cast<uint64_t>(esp_timer_get_time());
    FormatHex(report.data, report.length_data, incoming.advertising_data,
              sizeof(incoming.advertising_data), kAdvertisingBytes);

    if (fields != nullptr) {
        if (fields->name != nullptr && fields->name_len > 0) {
            CopyText(incoming.name, sizeof(incoming.name),
                     reinterpret_cast<const char*>(fields->name), fields->name_len);
        }
        FormatHex(fields->mfg_data, fields->mfg_data_len,
                  incoming.manufacturer_data, sizeof(incoming.manufacturer_data),
                  kManufacturerBytes);
        BuildServiceUuids(*fields, incoming.service_uuids,
                          sizeof(incoming.service_uuids));
    }

    std::lock_guard<std::mutex> lock(g_devices_mutex);
    size_t index = g_device_count;
    for (size_t i = 0; i < g_device_count; ++i) {
        if (std::strcmp(g_devices[i].address, incoming.address) == 0 &&
            std::strcmp(g_devices[i].address_type, incoming.address_type) == 0) {
            index = i;
            break;
        }
    }
    if (index == g_device_count) {
        if (g_device_count < kMaxDevices) {
            ++g_device_count;
        } else {
            index = 0;
            for (size_t i = 1; i < g_device_count; ++i) {
                if (g_devices[i].last_seen_us < g_devices[index].last_seen_us) index = i;
            }
        }
    }

    Device& stored = g_devices[index];
    if (incoming.name[0] != '\0') CopyText(stored.name, sizeof(stored.name), incoming.name, std::strlen(incoming.name));
    if (incoming.manufacturer_data[0] != '\0')
        CopyText(stored.manufacturer_data, sizeof(stored.manufacturer_data),
                 incoming.manufacturer_data, std::strlen(incoming.manufacturer_data));
    if (incoming.service_uuids[0] != '\0')
        CopyText(stored.service_uuids, sizeof(stored.service_uuids),
                 incoming.service_uuids, std::strlen(incoming.service_uuids));
    CopyText(stored.address, sizeof(stored.address), incoming.address, std::strlen(incoming.address));
    CopyText(stored.address_type, sizeof(stored.address_type),
             incoming.address_type, std::strlen(incoming.address_type));
    CopyText(stored.advertising_data, sizeof(stored.advertising_data),
             incoming.advertising_data, std::strlen(incoming.advertising_data));
    stored.rssi = incoming.rssi;
    stored.last_seen_us = incoming.last_seen_us;
}

esp_err_t BeginScan() {
    std::lock_guard<std::mutex> scan_lock(g_scan_mutex);
    if (!g_host_synced.load()) return ESP_ERR_INVALID_STATE;
    if (g_scanning.load()) return ESP_OK;

    uint8_t own_address_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_address_type);
    if (rc != 0) {
        g_scan_requested.store(false);
        g_last_error.store(rc);
        char detail[64]{};
        std::snprintf(detail, sizeof(detail), "ble_hs_id_infer_auto rc=%d", rc);
        chatai::diagnostics::Add("BLE_SCAN", "FAIL", detail);
        chatai::display::SetBluetoothStatus(
            chatai::display::BluetoothStatus::kError, GetDeviceCount(), rc);
        return ESP_FAIL;
    }

    ble_gap_disc_params parameters{};
    parameters.passive = 1;
    parameters.filter_duplicates = 0;

    rc = ble_gap_disc(own_address_type, kScanDurationMs, &parameters,
                      [](ble_gap_event* event, void*) -> int {
        switch (event->type) {
            case BLE_GAP_EVENT_DISC: {
                ble_hs_adv_fields fields{};
                const int parse_result = ble_hs_adv_parse_fields(
                    &fields, event->disc.data, event->disc.length_data);
                UpdateDevice(event->disc, parse_result == 0 ? &fields : nullptr);
                chatai::display::SetBluetoothStatus(
                    chatai::display::BluetoothStatus::kScanning, GetDeviceCount(), 0);
                break;
            }
            case BLE_GAP_EVENT_DISC_COMPLETE:
                g_scanning.store(false);
                g_scan_requested.store(false);
                chatai::display::SetBluetoothStatus(
                    chatai::display::BluetoothStatus::kComplete, GetDeviceCount(), 0);
                chatai::diagnostics::Add("BLE_SCAN", "COMPLETE", "BLE discovery completed");
                break;
            default:
                break;
        }
        return 0;
    }, nullptr);

    if (rc != 0) {
        g_scanning.store(false);
        g_scan_requested.store(false);
        g_last_error.store(rc);
        char detail[64]{};
        std::snprintf(detail, sizeof(detail), "ble_gap_disc rc=%d", rc);
        chatai::diagnostics::Add("BLE_SCAN", "FAIL", detail);
        chatai::display::SetBluetoothStatus(
            chatai::display::BluetoothStatus::kError, GetDeviceCount(), rc);
        ESP_LOGW(kTag, "Could not start BLE discovery: rc=%d", rc);
        return ESP_FAIL;
    }
    g_scanning.store(true);
    g_scan_requested.store(false);
    g_last_error.store(0);
    chatai::diagnostics::Add("BLE_SCAN", "STARTED", "passive=1 duration_ms=10000");
    chatai::display::SetBluetoothStatus(
        chatai::display::BluetoothStatus::kScanning, GetDeviceCount(), 0);
    ESP_LOGI(kTag, "Passive BLE scan started for %ld ms",
             static_cast<long>(kScanDurationMs));
    return ESP_OK;
}

void OnSync() {
    g_host_synced.store(true);
    chatai::diagnostics::Add("BLE_HOST", "SYNCED", "NimBLE host synchronized");
    ESP_LOGI(kTag, "NimBLE host synchronized");
    if (g_scan_requested.load()) {
        const esp_err_t result = BeginScan();
        if (result != ESP_OK) {
            g_scan_requested.store(false);
            chatai::display::SetBluetoothStatus(
                chatai::display::BluetoothStatus::kError, GetDeviceCount(),
                g_last_error.load());
            ESP_LOGW(kTag, "Deferred BLE scan could not start: %s",
                     esp_err_to_name(result));
        }
    } else {
        chatai::display::SetBluetoothStatus(
            chatai::display::BluetoothStatus::kReady, GetDeviceCount(), 0);
    }
}

void OnReset(int reason) {
    g_host_synced.store(false);
    g_scanning.store(false);
    g_scan_requested.store(false);
    g_last_error.store(reason);
    char detail[64]{};
    std::snprintf(detail, sizeof(detail), "NimBLE reset reason=%d", reason);
    chatai::diagnostics::Add("BLE_HOST", "RESET", detail);
    chatai::display::SetBluetoothStatus(
        chatai::display::BluetoothStatus::kError, GetDeviceCount(), reason);
    ESP_LOGW(kTag, "NimBLE host reset: reason=%d", reason);
}

void HostTask(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void InitializeNimbleTask(void*) {
    chatai::diagnostics::Add("BLE_INIT", "START", "nimble_port_init");
    const esp_err_t result = nimble_port_init();
    if (result != ESP_OK) {
        g_scan_requested.store(false);
        g_initializing.store(false);
        g_last_error.store(result);
        chatai::display::SetBluetoothStatus(
            chatai::display::BluetoothStatus::kError, GetDeviceCount(), result);
        chatai::diagnostics::Add("BLE_INIT", "FAIL", esp_err_to_name(result));
        ESP_LOGE(kTag, "NimBLE initialization failed: %s",
                 esp_err_to_name(result));
        vTaskDelete(nullptr);
        return;
    }

    ble_hs_cfg.reset_cb = OnReset;
    ble_hs_cfg.sync_cb = OnSync;
    g_initialized.store(true);
    chatai::diagnostics::Add("BLE_INIT", "OK", "NimBLE initialized; starting host task");
    ESP_LOGI(kTag, "NimBLE initialized in BLE worker task");
    nimble_port_freertos_init(HostTask);
    g_initializing.store(false);
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t StartScan() {
    bool start_now = false;
    {
        std::lock_guard<std::mutex> lock(g_init_mutex);
        if (g_scanning.load() || g_scan_requested.load()) return ESP_OK;
        {
            std::lock_guard<std::mutex> devices_lock(g_devices_mutex);
            g_devices = {};
            g_device_count = 0;
        }
        g_last_error.store(0);
        g_scan_requested.store(true);
        chatai::diagnostics::Add("BLE_SCAN", "REQUESTED", "StartScan accepted for initialization/scan");
        chatai::display::SetBluetoothStatus(
            chatai::display::BluetoothStatus::kInitializing, 0, 0);

        if (!g_initialized.load()) {
            if (!g_initializing.exchange(true)) {
                const BaseType_t created = xTaskCreate(
                    InitializeNimbleTask, "chatai_ble_init",
                    kInitTaskStackBytes, nullptr, kInitTaskPriority, nullptr);
                if (created != pdPASS) {
                    g_initializing.store(false);
                    g_scan_requested.store(false);
                    g_last_error.store(ESP_ERR_NO_MEM);
                    chatai::display::SetBluetoothStatus(
                        chatai::display::BluetoothStatus::kError, 0,
                        ESP_ERR_NO_MEM);
                    chatai::diagnostics::Add("BLE_INIT", "FAIL", "xTaskCreate returned no memory");
                    ESP_LOGE(kTag, "Could not create BLE initialization task");
                    return ESP_ERR_NO_MEM;
                }
            }
            return ESP_OK;
        }
        start_now = g_host_synced.load();
    }
    return start_now ? BeginScan() : ESP_OK;
}

esp_err_t StopScan() {
    g_scan_requested.store(false);
    if (!g_scanning.load()) return ESP_OK;
    const int result = ble_gap_disc_cancel();
    if (result != 0 && result != BLE_HS_EALREADY) {
        g_last_error.store(result);
        return ESP_FAIL;
    }
    g_scanning.store(false);
    chatai::display::SetBluetoothStatus(
        chatai::display::BluetoothStatus::kComplete, GetDeviceCount(), 0);
    return ESP_OK;
}

bool IsInitialized() { return g_initialized.load(); }
bool IsReady() { return g_host_synced.load(); }
bool IsScanning() { return g_scanning.load(); }
bool IsScanRequested() { return g_scan_requested.load(); }
int GetLastError() { return g_last_error.load(); }

size_t GetDeviceCount() {
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    return g_device_count;
}

size_t GetDevices(Device* output, size_t capacity) {
    if (output == nullptr || capacity == 0) return 0;
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    const size_t count = std::min(g_device_count, capacity);
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    for (size_t i = 0; i < count; ++i) {
        output[i] = g_devices[i];
        const uint64_t elapsed = now >= g_devices[i].last_seen_us
            ? now - g_devices[i].last_seen_us : 0;
        output[i].last_seen_seconds = static_cast<uint32_t>(elapsed / 1000000ULL);
        output[i].last_seen_us = 0;
    }
    return count;
}

}  // namespace chatai::bluetooth
