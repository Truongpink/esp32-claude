#pragma once

#include <esp_err.h>
#include <string>

namespace chatai::network {

class NetworkManager {
public:
    static esp_err_t ClearSavedCredentials();
    static esp_err_t Initialize();
    static esp_err_t Connect(const std::string& ssid, const std::string& password);
    static bool IsConnected();
    static std::string GetIpAddress();
    static bool IsProvisioningActive();
    static bool HasSavedCredentials();
    static std::string GetProvisioningSsid();
};

}  // namespace chatai::network
