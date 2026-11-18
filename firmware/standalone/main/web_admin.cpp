#include "web_admin.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "gemini_settings.h"

namespace {
constexpr char kTag[] = "ChatAI-Admin";
constexpr size_t kMaxRequestBody = 512;
constexpr size_t kMaxApiKeyLength = 256;
std::string g_csrf_token;
std::mutex g_ota_mutex;

const char kAdminPage[] = R"html(
<!doctype html>
<html lang="vi">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>ChatAI TPIOS - Web Admin</title>
<style>
:root{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color-scheme:light dark}
body{max-width:560px;margin:36px auto;padding:0 18px;line-height:1.5}
main{border:1px solid #8885;border-radius:16px;padding:22px}
h1{font-size:1.45rem;margin-top:0}
label{display:block;font-weight:600;margin:18px 0 7px}
input{box-sizing:border-box;width:100%;padding:13px;border:1px solid #8888;border-radius:9px;font:inherit}
button{padding:11px 16px;border:0;border-radius:9px;font:inherit;font-weight:600;margin:14px 8px 0 0}
small,.muted{opacity:.72}
#result{min-height:1.5em;margin-top:14px}
</style>
</head>
<body>
<main>
<h1>ChatAI TPIOS</h1>
<p class="muted">Cấu hình Gemini Live trên thiết bị.</p>
<p id="status">Đang kiểm tra trạng thái...</p>
<label for="apiKey">Gemini API Key</label>
<input id="apiKey" type="password" maxlength="256" autocomplete="new-password"
       autocapitalize="off" spellcheck="false" placeholder="Nhập API Key mới">
<small>Khóa đã lưu không được gửi ngược về trình duyệt. Để thay khóa, nhập khóa mới.</small>
<div>
<button id="save">Lưu API Key</button>
<button id="remove" hidden>Xóa API Key</button>
</div>
<hr>
<h2>Cập nhật Firmware OTA</h2>
<p class="muted">Chọn file ChatAI_TPIOS.bin. Giữ thiết bị cắm nguồn trong suốt quá trình cập nhật.</p>
<label for="firmware">Firmware BIN</label>
<input id="firmware" type="file" accept=".bin,application/octet-stream">
<button id="updateFirmware">Cài đặt firmware</button>
<p id="otaResult" role="status"></p>
<p id="result" role="status"></p>
</main>
<script>
const csrfToken = "__CSRF_TOKEN__";
const statusText = document.getElementById("status");
const resultText = document.getElementById("result");
const keyInput = document.getElementById("apiKey");
const saveButton = document.getElementById("save");
const removeButton = document.getElementById("remove");
const firmwareInput = document.getElementById("firmware");
const updateFirmwareButton = document.getElementById("updateFirmware");
const otaResult = document.getElementById("otaResult");

updateFirmwareButton.addEventListener("click", async () => {
  const file = firmwareInput.files && firmwareInput.files[0];
  if (!file) { otaResult.textContent = "Vui lòng chọn file BIN."; return; }
  if (!file.name.toLowerCase().endsWith(".bin")) {
    otaResult.textContent = "Chỉ chấp nhận file firmware .bin.";
    return;
  }
  if (!confirm("Cài đặt firmware mới và khởi động lại ESP32?")) return;
  updateFirmwareButton.disabled = true;
  otaResult.textContent = "Đang tải và ghi firmware. Không tắt nguồn thiết bị...";
  try {
    const response = await fetch("/api/ota", {
      method: "POST",
      headers: {"X-ChatAI-CSRF": csrfToken},
      body: file,
      cache: "no-store"
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(data.error || ("OTA thất bại (HTTP " + response.status + ")"));
    otaResult.textContent = data.message || "Firmware hợp lệ. ESP32 đang khởi động lại...";
  } catch (error) {
    otaResult.textContent = error.message + " Nếu thiết bị đã khởi động lại, hãy chờ rồi tải lại trang.";
    updateFirmwareButton.disabled = false;
  }
});

async function refreshStatus() {
  const response = await fetch("/api/gemini/status", {cache:"no-store"});
  if (!response.ok) throw new Error("Không đọc được trạng thái (HTTP " + response.status + ")");
  const data = await response.json();
  statusText.textContent = data.key_configured ? "Trạng thái: API Key đã được lưu." :
    "Trạng thái: Chưa cấu hình API Key.";
  removeButton.hidden = !data.key_configured;
}
async function sendKeyRequest(method, body) {
  const options = {method, cache:"no-store", headers:{"X-ChatAI-CSRF":csrfToken}};
  if (body !== undefined) {
    options.headers["Content-Type"] = "application/json";
    options.body = JSON.stringify(body);
  }
  const response = await fetch("/api/gemini/key", options);
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(data.error || ("Yêu cầu thất bại (HTTP " + response.status + ")"));
  return data;
}
saveButton.addEventListener("click", async () => {
  const apiKey = keyInput.value.trim();
  if (!apiKey) { resultText.textContent = "Vui lòng nhập API Key."; return; }
  saveButton.disabled = true;
  resultText.textContent = "Đang lưu...";
  try {
    const data = await sendKeyRequest("POST", {api_key:apiKey});
    keyInput.value = "";
    resultText.textContent = data.message || "Đã lưu API Key.";
    await refreshStatus();
  } catch (error) {
    resultText.textContent = error.message;
  } finally { saveButton.disabled = false; }
});
removeButton.addEventListener("click", async () => {
  if (!confirm("Xóa Gemini API Key đang lưu trên thiết bị?")) return;
  removeButton.disabled = true;
  resultText.textContent = "Đang xóa...";
  try {
    const data = await sendKeyRequest("DELETE");
    resultText.textContent = data.message || "Đã xóa API Key.";
    await refreshStatus();
  } catch (error) {
    resultText.textContent = error.message;
  } finally { removeButton.disabled = false; }
});
refreshStatus().catch(error => { statusText.textContent = error.message; });
</script>
</body>
</html>
)html";

void SetNoStore(httpd_req_t* request) {
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
}

esp_err_t SendJson(httpd_req_t* request, const char* status, const char* json) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    SetNoStore(request);
    return httpd_resp_sendstr(request, json);
}

esp_err_t SendError(httpd_req_t* request, const char* status, const char* message) {
    const std::string body = std::string("{\"error\":\"") + message + "\"}";
    return SendJson(request, status, body.c_str());
}

bool IsSameOrigin(httpd_req_t* request) {
    const size_t origin_length = httpd_req_get_hdr_value_len(request, "Origin");
    const size_t host_length = httpd_req_get_hdr_value_len(request, "Host");
    if (origin_length == 0 || host_length == 0 ||
        origin_length >= 160 || host_length >= 96) return false;

    char origin[160]{};
    char host[96]{};
    if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }

    const std::string expected = std::string("http://") + host;
    if (origin == expected) return true;
    return expected.find(":80") == std::string::npos &&
           origin == expected + ":80";
}

bool IsAuthorizedMutation(httpd_req_t* request) {
    if (!IsSameOrigin(request)) return false;
    const size_t token_length = httpd_req_get_hdr_value_len(request, "X-ChatAI-CSRF");
    if (token_length == 0 || token_length >= 64) return false;
    char token[64]{};
    if (httpd_req_get_hdr_value_str(request, "X-ChatAI-CSRF", token, sizeof(token)) != ESP_OK) {
        return false;
    }
    return g_csrf_token == token;
}

bool ReadBody(httpd_req_t* request, std::string& body) {
    if (request->content_len == 0 || request->content_len > kMaxRequestBody) return false;
    body.assign(request->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int count = httpd_req_recv(
            request, body.data() + received, body.size() - received);
        if (count <= 0) return false;
        received += static_cast<size_t>(count);
    }
    return true;
}

bool IsValidApiKey(const char* value) {
    if (value == nullptr) return false;
    const size_t length = std::strlen(value);
    if (length < 8 || length > kMaxApiKeyLength) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c < 0x21 || c > 0x7e) return false;
    }
    return true;
}

esp_err_t AdminPageHandler(httpd_req_t* request) {
    std::string page(kAdminPage);
    const size_t marker = page.find("__CSRF_TOKEN__");
    if (marker == std::string::npos) return ESP_FAIL;
    page.replace(marker, std::strlen("__CSRF_TOKEN__"), g_csrf_token);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    SetNoStore(request);
    httpd_resp_set_hdr(request, "Content-Security-Policy",
        "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
        "connect-src 'self'; base-uri 'none'; frame-ancestors 'none'");
    return httpd_resp_send(request, page.c_str(), page.size());
}

esp_err_t GeminiStatusHandler(httpd_req_t* request) {
    const char* status = chatai::settings::HasGeminiApiKey()
        ? "{\"key_configured\":true}"
        : "{\"key_configured\":false}";
    return SendJson(request, "200 OK", status);
}

esp_err_t GeminiKeyPostHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yeu cau khong hop le");
    }

    std::string body;
    if (!ReadBody(request, body)) {
        return SendError(request, "400 Bad Request", "Du lieu gui len khong hop le");
    }

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!cJSON_IsObject(root)) {
        if (root != nullptr) cJSON_Delete(root);
        return SendError(request, "400 Bad Request", "JSON khong hop le");
    }

    const cJSON* key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    const bool valid = cJSON_IsString(key) && IsValidApiKey(key->valuestring);
    const std::string api_key = valid ? key->valuestring : "";
    cJSON_Delete(root);
    if (!valid) {
        return SendError(request, "400 Bad Request", "Gemini API Key khong hop le");
    }

    const esp_err_t result = chatai::settings::SaveGeminiApiKey(api_key);
    if (result != ESP_OK) {
        return SendError(request, "500 Internal Server Error", "Khong the luu API Key vao NVS");
    }
    return SendJson(request, "200 OK", "{\"message\":\"Da luu Gemini API Key.\"}");
}

esp_err_t OtaUploadHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yeu cau khong hop le");
    }
    if (request->content_len < 4096) {
        return SendError(request, "400 Bad Request", "Firmware BIN rong hoac qua nho");
    }

    std::unique_lock<std::mutex> lock(g_ota_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return SendError(request, "409 Conflict", "Dang co mot qua trinh OTA khac");
    }

    const esp_partition_t* ota0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0");
    const esp_partition_t* ota1 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1");
    const esp_partition_t* otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata");
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);

    if (ota0 == nullptr || ota1 == nullptr || otadata == nullptr ||
        running == nullptr || target == nullptr || target == running ||
        (target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         target->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) {
        return SendError(request, "503 Service Unavailable",
                         "Khong tim thay phan vung OTA hai slot tuong thich");
    }
    if (static_cast<size_t>(request->content_len) > target->size) {
        return SendError(request, "413 Content Too Large",
                         "Firmware BIN lon hon phan vung OTA trong");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t result = esp_ota_begin(
        target, static_cast<size_t>(request->content_len), &handle);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_begin failed: %s", esp_err_to_name(result));
        return SendError(request, "500 Internal Server Error",
                         "Khong the bat dau ghi firmware");
    }

    uint8_t buffer[1024];
    size_t remaining = static_cast<size_t>(request->content_len);
    size_t written = 0;
    while (remaining > 0) {
        const size_t wanted = std::min(remaining, sizeof(buffer));
        const int received = httpd_req_recv(
            request, reinterpret_cast<char*>(buffer), wanted);
        if (received <= 0) {
            esp_ota_abort(handle);
            ESP_LOGE(kTag, "OTA upload interrupted after %u bytes",
                     static_cast<unsigned>(written));
            return SendError(request, "400 Bad Request",
                             "Qua trinh tai firmware bi gian doan");
        }

        result = esp_ota_write(handle, buffer, static_cast<size_t>(received));
        if (result != ESP_OK) {
            esp_ota_abort(handle);
            ESP_LOGE(kTag, "esp_ota_write failed: %s", esp_err_to_name(result));
            return SendError(request, "500 Internal Server Error",
                             "Khong the ghi firmware vao OTA slot");
        }
        remaining -= static_cast<size_t>(received);
        written += static_cast<size_t>(received);
    }

    result = esp_ota_end(handle);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "OTA image validation failed: %s", esp_err_to_name(result));
        return SendError(request, "400 Bad Request",
                         "Firmware BIN khong hop le hoac khong tuong thich");
    }

    result = esp_ota_set_boot_partition(target);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Could not select OTA boot partition: %s",
                 esp_err_to_name(result));
        return SendError(request, "500 Internal Server Error",
                         "Khong the chon firmware moi de khoi dong");
    }

    ESP_LOGI(kTag, "OTA image accepted: %u bytes -> %s",
             static_cast<unsigned>(written), target->label);
    const esp_err_t response_result = SendJson(
        request, "200 OK",
        "{\"message\":\"Firmware da duoc xac thuc va cai dat. ESP32 dang khoi dong lai.\"}");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return response_result;
}

esp_err_t GeminiKeyDeleteHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yeu cau khong hop le");
    }
    const esp_err_t result = chatai::settings::DeleteGeminiApiKey();
    if (result != ESP_OK) {
        return SendError(request, "500 Internal Server Error", "Khong the xoa API Key khoi NVS");
    }
    return SendJson(request, "200 OK", "{\"message\":\"Da xoa Gemini API Key.\"}");
}
}  // namespace

namespace chatai::web_admin {

esp_err_t RegisterRoutes(httpd_handle_t server, const std::string& csrf_token) {
    if (server == nullptr || csrf_token.empty()) return ESP_ERR_INVALID_ARG;
    g_csrf_token = csrf_token;

    httpd_uri_t admin_uri{};
    admin_uri.uri = "/admin";
    admin_uri.method = HTTP_GET;
    admin_uri.handler = AdminPageHandler;

    httpd_uri_t status_uri{};
    status_uri.uri = "/api/gemini/status";
    status_uri.method = HTTP_GET;
    status_uri.handler = GeminiStatusHandler;

    httpd_uri_t save_uri{};
    save_uri.uri = "/api/gemini/key";
    save_uri.method = HTTP_POST;
    save_uri.handler = GeminiKeyPostHandler;

    httpd_uri_t delete_uri{};
    delete_uri.uri = "/api/gemini/key";
    delete_uri.method = HTTP_DELETE;
    delete_uri.handler = GeminiKeyDeleteHandler;

    httpd_uri_t ota_uri{};
    ota_uri.uri = "/api/ota";
    ota_uri.method = HTTP_POST;
    ota_uri.handler = OtaUploadHandler;

    esp_err_t result = httpd_register_uri_handler(server, &admin_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &status_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &save_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &delete_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &ota_uri);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Could not register Web Admin routes: %s", esp_err_to_name(result));
    }
    return result;
}

}  // namespace chatai::web_admin
