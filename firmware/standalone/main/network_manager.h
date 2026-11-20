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
    static bool IsInternetAvailable();
    static std::string GetIpAddress();
    static std::string GetGatewayAddress();
    static std::string GetSsid();
    static int GetRssi();
    static std::string GetDnsAddress();
    static std::string GetSoftApIpAddress();
    static bool IsProvisioningActive();
    static bool HasSavedCredentials();
    static std::string GetProvisioningSsid();
};

}  // namespace chatai::network
