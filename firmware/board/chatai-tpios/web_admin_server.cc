#include "web_admin_server.h"

#include "settings.h"
#include <esp_http_server.h>
#include <esp_log.h>
#include <wifi_manager.h>
#include <cJSON.h>
#include <mutex>
#include <string>
#include <cstring>

namespace {
constexpr char TAG[] = "WebAdmin";
constexpr size_t kMaxBody = 4096;
httpd_handle_t server = nullptr;
std::mutex server_mutex;

const char kIndexHtml[] = R"HTML(
<!doctype html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ChatAI TPIOS</title>
<style>
:root{color-scheme:dark;font:16px system-ui;background:#10131a;color:#edf1f7}
body{max-width:680px;margin:32px auto;padding:0 16px}
h1{font-size:26px;margin-bottom:4px}p,small{color:#aab4c4}
section{background:#1b2230;border:1px solid #303b4d;border-radius:14px;padding:18px;margin:18px 0}
label{display:block;margin:14px 0 6px}input,textarea{box-sizing:border-box;width:100%;padding:12px;border:1px solid #46536a;border-radius:8px;background:#111722;color:#fff;font:inherit}
textarea{min-height:130px;resize:vertical}.row{display:flex;gap:10px;align-items:center}.row input{width:auto}
button{border:0;border-radius:9px;padding:12px 18px;background:#4e8cff;color:#fff;font-weight:700;font-size:16px}
#status{margin-top:12px;white-space:pre-wrap}
</style></head><body>
<h1>ChatAI TPIOS</h1><p>Quản trị thiết bị trong mạng Wi-Fi nội bộ</p>
<section><h2>Kết nối</h2><div id="status">Đang tải trạng thái...</div></section>
<section><h2>Gemini Live</h2>
<form id="config">
<label>Gemini API key</label><input id="key" type="password" autocomplete="off" placeholder="Để trống nếu muốn giữ key đã lưu">
<small>API key được lưu trong NVS của thiết bị và không hiển thị lại trên trang.</small>
<label>Model</label><input id="model" maxlength="96" value="gemini-3.8-live">
<label>System prompt</label><textarea id="prompt" maxlength="1024"></textarea>
<label class="row"><input id="enabled" type="checkbox"> Bật Gemini Live</label>
<button type="submit">Lưu cấu hình</button><p id="result"></p>
</form></section>
<p><small>Cấu hình Gemini có hiệu lực sau khi khởi động lại thiết bị.</small></p>
<script>
const $=id=>document.getElementById(id);
async function load(){
 try{
  const [s,c]=await Promise.all([fetch('/api/status',{cache:'no-store'}),fetch('/api/config',{cache:'no-store'})]);
  if(!s.ok||!c.ok)throw Error('Không đọc được trạng thái thiết bị');
  const status=await s.json(),cfg=await c.json();
  $('status').textContent='Wi-Fi: '+status.ssid+'\nIP: '+status.ip+'\nGemini: '+(status.enabled?'Đang bật':'Đang tắt')+'\nAPI key: '+(status.key_configured?'Đã lưu':'Chưa cấu hình');
  $('model').value=cfg.model;$('prompt').value=cfg.system_prompt;$('enabled').checked=cfg.enabled;
 }catch(e){$('status').textContent=e.message}
}
$('config').addEventListener('submit',async e=>{
 e.preventDefault();$('result').textContent='Đang lưu...';
 const body={model:$('model').value,system_prompt:$('prompt').value,enabled:$('enabled').checked};
 if($('key').value)body.api_key=$('key').value;
 try{
  const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const data=await r.json();if(!r.ok)throw Error(data.error||'Không lưu được cấu hình');
  $('key').value='';$('result').textContent=data.message;await load();
 }catch(e){$('result').textContent=e.message}
});
load();
</script></body></html>
)HTML";

void SendJson(httpd_req_t* req, cJSON* root, const char* status = "200 OK") {
    char* encoded = cJSON_PrintUnformatted(root);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, encoded ? encoded : "{}");
    if (encoded) cJSON_free(encoded);
    cJSON_Delete(root);
}

void SendError(httpd_req_t* req, const char* status, const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message);
    SendJson(req, root, status);
}

esp_err_t IndexHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, kIndexHtml);
}

esp_err_t StatusHandler(httpd_req_t* req) {
    auto& wifi = WifiManager::GetInstance();
    Settings settings("gemini", false);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ip", wifi.GetIpAddress().c_str());
    cJSON_AddStringToObject(root, "ssid", wifi.GetSsid().c_str());
    cJSON_AddBoolToObject(root, "enabled", settings.GetBool("enabled", false));
    cJSON_AddBoolToObject(root, "key_configured", !settings.GetString("api_key").empty());
    SendJson(req, root);
    return ESP_OK;
}

esp_err_t ConfigGetHandler(httpd_req_t* req) {
    Settings settings("gemini", false);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", settings.GetString("model", "gemini-3.8-live").c_str());
    cJSON_AddStringToObject(root, "system_prompt", settings.GetString(
        "system_prompt",
        "You are ChatAI TPIOS, a helpful Vietnamese voice assistant. Speak naturally in Vietnamese unless the user asks for another language. Keep replies concise and conversational."
    ).c_str());
    cJSON_AddBoolToObject(root, "enabled", settings.GetBool("enabled", false));
    SendJson(req, root);
    return ESP_OK;
}

bool IsSameOrigin(httpd_req_t* req) {
    const size_t length = httpd_req_get_hdr_value_len(req, "Origin");
    if (length == 0) return true; // Native tools may omit Origin; browsers send it for JSON POST.
    if (length >= 128) return false;
    char origin[128] = {};
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) return false;
    const std::string expected = "http://" + WifiManager::GetInstance().GetIpAddress();
    return expected == origin;
}

esp_err_t ConfigPostHandler(httpd_req_t* req) {
    if (!IsSameOrigin(req)) {
        SendError(req, "403 Forbidden", "Cross-origin request rejected");
        return ESP_OK;
    }
    if (req->content_len == 0 || req->content_len > kMaxBody) {
        SendError(req, "400 Bad Request", "Invalid configuration size");
        return ESP_OK;
    }

    std::string body(req->content_len, '\0');
    size_t received = 0;
    while (received < body.size()) {
        const int count = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (count <= 0) {
            SendError(req, "400 Bad Request", "Incomplete request body");
            return ESP_OK;
        }
        received += static_cast<size_t>(count);
    }

    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        SendError(req, "400 Bad Request", "Invalid JSON");
        return ESP_OK;
    }

    const cJSON* key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    const cJSON* model = cJSON_GetObjectItemCaseSensitive(root, "model");
    const cJSON* prompt = cJSON_GetObjectItemCaseSensitive(root, "system_prompt");
    const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");

    const bool valid =
        (!key || (cJSON_IsString(key) && std::string(key->valuestring).size() <= 256)) &&
        cJSON_IsString(model) && std::strlen(model->valuestring) > 0 &&
        std::strlen(model->valuestring) <= 96 &&
        cJSON_IsString(prompt) && std::strlen(prompt->valuestring) <= 1024 &&
        cJSON_IsBool(enabled);
    if (!valid) {
        cJSON_Delete(root);
        SendError(req, "400 Bad Request", "Invalid model, prompt, key or enabled value");
        return ESP_OK;
    }

    Settings existing("gemini", false);
    const bool has_key = !existing.GetString("api_key").empty() ||
                         (key && key->valuestring && key->valuestring[0] != '\0');
    if (cJSON_IsTrue(enabled) && !has_key) {
        cJSON_Delete(root);
        SendError(req, "400 Bad Request", "Enter an API key before enabling Gemini Live");
        return ESP_OK;
    }

    {
        Settings settings("gemini", true);
        if (key && key->valuestring && key->valuestring[0] != '\0') {
            settings.SetString("api_key", key->valuestring);
        }
        settings.SetString("model", model->valuestring);
        settings.SetString("system_prompt", prompt->valuestring);
        settings.SetBool("enabled", cJSON_IsTrue(enabled));
    }
    cJSON_Delete(root);

    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "message", "Đã lưu. Khởi động lại thiết bị để áp dụng cấu hình Gemini.");
    SendJson(req, response);
    return ESP_OK;
}

} // namespace

void WebAdminServer::Start() {
    std::lock_guard<std::mutex> lock(server_mutex);
    if (server) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 4;
    config.stack_size = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        server = nullptr;
        ESP_LOGE(TAG, "Could not start local Web Admin");
        return;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = IndexHandler, .user_ctx = nullptr},
        {.uri = "/api/status", .method = HTTP_GET, .handler = StatusHandler, .user_ctx = nullptr},
        {.uri = "/api/config", .method = HTTP_GET, .handler = ConfigGetHandler, .user_ctx = nullptr},
        {.uri = "/api/config", .method = HTTP_POST, .handler = ConfigPostHandler, .user_ctx = nullptr},
    };
    bool ok = true;
    for (const auto& route : routes) {
        if (httpd_register_uri_handler(server, &route) != ESP_OK) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        httpd_stop(server);
        server = nullptr;
        ESP_LOGE(TAG, "Could not register local Web Admin routes");
        return;
    }
    ESP_LOGI(TAG, "Local Web Admin started on port 80");
}

void WebAdminServer::Stop() {
    std::lock_guard<std::mutex> lock(server_mutex);
    if (!server) return;
    httpd_stop(server);
    server = nullptr;
    ESP_LOGI(TAG, "Local Web Admin stopped");
}
