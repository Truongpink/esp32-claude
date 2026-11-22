#pragma once

#include <esp_err.h>
#include <esp_http_server.h>
#include <string>

namespace chatai::web_admin {

esp_err_t RegisterRoutes(httpd_handle_t server, const std::string& csrf_token);

}  // namespace chatai::web_admin
