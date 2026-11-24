#include "web_admin.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "gemini_settings.h"
#include "display_manager.h"
#include "network_manager.h"
#include "firmware_info.h"
#include "diagnostic_manager.h"
#include "audio_manager.h"
#include "bluetooth_manager.h"
#include "chatai_xiaozhi_client.h"

namespace {
constexpr char kTag[] = "ChatAI-Admin";
constexpr size_t kMaxRequestBody = 8192;
constexpr size_t kMaxApiKeyLength = 256;
std::string g_csrf_token;
std::mutex g_ota_mutex;
std::atomic<bool> g_restart_pending{false};

const char kAdminPage[] = R"html(
<!doctype html>
<html lang="vi">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>ChatAI TPIOS · Web Admin</title>
<style>
:root{color-scheme:dark;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#090914;color:#f4f1ff;font-synthesis:none}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;padding:20px 16px calc(112px + env(safe-area-inset-bottom));background:radial-gradient(ellipse at 50% -10%,#38206a 0,transparent 48%),linear-gradient(160deg,#0c0b19,#111020 58%,#090914)}
main{width:100%;max-width:760px;margin:0 auto}
header{display:flex;align-items:center;gap:13px;margin:0 2px 22px}
.brand-mark{width:48px;height:48px;display:grid;place-items:center;border-radius:16px;background:linear-gradient(145deg,#9b5cff,#6034d6);box-shadow:0 8px 28px #8149ff45;font-size:24px}
h1{font-size:21px;line-height:1.2;margin:0;font-weight:760;letter-spacing:-.35px}
.subtitle{margin:5px 0 0;color:#a9a2c4;font-size:13px}
.card{background:linear-gradient(145deg,#1b1930f5,#151426f5);border:1px solid #ffffff14;border-radius:21px;padding:20px;margin:0 0 15px;box-shadow:0 16px 40px #0003}
.card-head{display:flex;align-items:center;gap:11px;margin-bottom:15px}
.card-icon{width:37px;height:37px;display:grid;place-items:center;border-radius:12px;background:#8c5bff20;color:#c4a5ff;font-size:18px}
h2{font-size:16px;margin:0;font-weight:700}
.hint{font-size:12px;color:#a9a2c4;margin:4px 0 0;line-height:1.5}
#status{font-size:13px;color:#d5c9ff;margin:0 0 10px}
#deviceStatus{font-size:13px;line-height:1.55;color:#e9e3ff;background:#ffffff08;border:1px solid #ffffff10;border-radius:13px;padding:12px;margin:0}
label{display:block;font-size:13px;font-weight:650;color:#e4def6;margin:16px 0 8px}
input[type=password],input[type=file]{width:100%;min-width:0;padding:13px 14px;border:1px solid #ffffff20;border-radius:12px;background:#0d0c19;color:#f5f1ff;font:inherit;font-size:14px;outline:none}
input:focus{border-color:#a477ff;box-shadow:0 0 0 3px #8c5bff24}
small,.muted{font-size:11px;line-height:1.5;color:#a9a2c4}
button{border:0;border-radius:12px;padding:12px 16px;background:linear-gradient(135deg,#9a5cff,#6739d9);color:white;font:inherit;font-size:13px;font-weight:700;cursor:pointer;box-shadow:0 7px 18px #7948dc35}
button:disabled{opacity:.55;cursor:wait}
button.secondary{background:#ffffff10;border:1px solid #ffffff1a;box-shadow:none;color:#e9e3ff}
.actions{display:flex;gap:9px;flex-wrap:wrap;margin-top:13px}
button.danger{background:linear-gradient(135deg,#b74a5a,#852c49);box-shadow:0 7px 18px #a8324a35}
hr{border:0;border-top:1px solid #ffffff12;margin:18px 0}
#result,#otaResult{font-size:12px;line-height:1.5;color:#d8caff;min-height:1.4em;margin:12px 0 0;overflow-wrap:anywhere}
#firmwareStatus{font-size:12px;line-height:1.55;color:#d9d2f0;margin:0 0 12px;overflow-wrap:anywhere}
#otaFileInfo{margin:9px 0}
.progress{height:8px;background:#ffffff12;border-radius:20px;overflow:hidden;margin-top:13px}
#otaProgress{height:100%;width:0;background:linear-gradient(90deg,#9b5cff,#55d6c2);transition:width .12s}
.tabbar{position:fixed;left:50%;bottom:0;transform:translateX(-50%);width:min(100%,760px);display:grid;grid-template-columns:repeat(7,minmax(0,1fr));gap:4px;padding:8px 10px calc(8px + env(safe-area-inset-bottom));border:1px solid #ffffff18;border-bottom:0;border-radius:20px 20px 0 0;background:#111020ed;backdrop-filter:blur(20px);z-index:20;box-shadow:0 -10px 35px #0005}
.tabbar button{min-width:0;min-height:54px;padding:5px 1px;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:3px;border-radius:12px;background:transparent;border:1px solid transparent;box-shadow:none;color:#aaa2c5;font-size:9px;line-height:1.15;white-space:normal;overflow-wrap:anywhere}
.tabbar button .tab-icon{font-size:17px;line-height:1.1}
.tabbar button[aria-selected="true"]{background:linear-gradient(145deg,#7043c9,#4b3291);border-color:#b89aff55;color:#fff}
.metric-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;margin-top:14px}
.metric{min-width:0;padding:12px;border:1px solid #ffffff12;border-radius:14px;background:#ffffff07}
.metric span{display:block;color:#aaa2c5;font-size:11px;margin-bottom:5px}
.metric strong{display:block;font-size:13px;overflow-wrap:anywhere;font-weight:650}
textarea{width:100%;min-height:220px;resize:vertical;padding:13px;border:1px solid #ffffff20;border-radius:13px;background:#0d0c19;color:#f5f1ff;font:13px/1.55 sans-serif;outline:none}
select,input[type=text]{width:100%;min-width:0;padding:13px;border:1px solid #ffffff20;border-radius:12px;background:#0d0c19;color:#f5f1ff;font:inherit}
@media(min-width:700px){.tabbar{bottom:18px;border:1px solid #ffffff18;border-radius:18px;width:min(720px,calc(100% - 32px))}body{padding-bottom:110px}.metric-grid{grid-template-columns:repeat(3,minmax(0,1fr))}}
.tab-panel[hidden]{display:none}
.ble-filter-row{display:grid;grid-template-columns:minmax(0,1fr) 130px;gap:8px;margin-top:14px}
.ble-device-list{display:grid;gap:10px;margin-top:14px}
.ble-device{min-width:0;padding:13px;border:1px solid #ffffff14;border-radius:14px;background:#ffffff07}
.ble-device-head{display:flex;align-items:flex-start;justify-content:space-between;gap:10px}
.ble-device-name{font-size:13px;font-weight:700;overflow-wrap:anywhere}
.ble-device-address{margin-top:4px;color:#aaa2c5;font:11px/1.4 ui-monospace,monospace;overflow-wrap:anywhere}
.ble-rssi{flex:none;padding:5px 8px;border:1px solid #ffffff18;border-radius:999px;color:#d9d2f0;font-size:10px;white-space:nowrap}
.ble-rssi-track{height:5px;margin-top:11px;background:#ffffff12;border-radius:99px;overflow:hidden}
.ble-rssi-fill{height:100%;border-radius:99px;background:linear-gradient(90deg,#55d6c2,#9b5cff)}
.ble-device-meta{display:grid;gap:5px;margin-top:10px;color:#aaa2c5;font-size:11px;line-height:1.45;overflow-wrap:anywhere}
.ble-device-meta strong{color:#e9e3ff;font-weight:650}
@media(max-width:390px){.ble-filter-row{grid-template-columns:minmax(0,1fr) 112px}.tabbar button{font-size:8px}}
.overview-panel{padding:17px;}
.overview-top{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:13px}
.overview-top .card-head{margin:0;min-width:0}
.overview-refresh{flex:none;padding:9px 11px}
.overview-hero{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:15px;border:1px solid #8f70d833;border-radius:16px;background:linear-gradient(110deg,#6842a52b,#ffffff05);margin-bottom:13px}
.overview-eyebrow{display:block;color:#b9a6e8;font-size:9px;font-weight:750;letter-spacing:1px;margin-bottom:5px}
.overview-hero strong{display:block;font-size:17px;line-height:1.25}
.overview-hero p{margin:5px 0 0;color:#c0b9d6;font-size:11px;line-height:1.5}
.overview-chip{flex:none;max-width:112px;padding:6px 8px;border-radius:9px;background:#ffffff0d;color:#d7c9f5;font-size:10px;text-align:center;overflow-wrap:anywhere}
.overview-grid{display:grid;grid-template-columns:1fr;gap:10px}
.overview-group{min-width:0;padding:13px;border:1px solid #ffffff12;border-radius:15px;background:#ffffff06}
.overview-group-title{display:flex;align-items:center;gap:8px;margin-bottom:9px}
.overview-group-title h3{font-size:13px;font-weight:720;margin:0}
.overview-group-icon{width:25px;height:25px;display:grid;place-items:center;border-radius:8px;background:#8c5bff20;color:#c4a5ff;font-size:14px}
.overview-items{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));column-gap:12px;row-gap:10px}
.overview-item{min-width:0}
.overview-item span{display:block;color:#a9a2c4;font-size:10px;margin-bottom:3px}
.overview-item strong{display:block;color:#f0ebff;font-size:12px;font-weight:650;overflow-wrap:anywhere;line-height:1.35}
.overview-note{margin:10px 0 0;color:#9992b3;font-size:10px;line-height:1.45}
.overview-footer{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-top:13px}
#overviewUpdated{color:#89829f;font-size:10px}
.overview-footer .danger{padding:10px 12px;font-size:11px}
.overview-panel #status{display:none}
@media(min-width:600px){.overview-grid{grid-template-columns:repeat(2,minmax(0,1fr))}.overview-panel{padding:20px}}
@media(max-width:360px){.overview-panel{padding:13px}.overview-items{column-gap:8px}.overview-footer{align-items:flex-start;flex-direction:column}}
.foot{text-align:center;color:#77718f;font-size:11px;margin:20px 0 0}
@media(min-width:600px){body{padding-top:44px}}
</style>
</head>
<body>
<main>
<header>
<div class="brand-mark" aria-hidden="true">✦</div>
<div><h1>ChatAI TPIOS</h1><p class="subtitle">Trung tâm điều khiển thiết bị</p></div>
</header>
<nav class="tabbar" role="tablist" aria-label="Các mục quản trị">
<button type="button" role="tab" data-tab="overview" aria-selected="true"><span class="tab-icon">⌂</span><span>Tổng quan</span></button>
<button type="button" role="tab" data-tab="wifi" aria-selected="false"><span class="tab-icon">⌁</span><span>Wi-Fi</span></button>
<button type="button" role="tab" data-tab="bluetooth" aria-selected="false"><span class="tab-icon">ᛒ</span><span>Bluetooth</span></button>
<button type="button" role="tab" data-tab="gemini" aria-selected="false"><span class="tab-icon">✧</span><span>Gemini (đã tắt)</span></button>
<button type="button" role="tab" data-tab="audio" aria-selected="false"><span class="tab-icon">♫</span><span>Âm thanh</span></button>
<button type="button" role="tab" data-tab="ota" aria-selected="false"><span class="tab-icon">⇧</span><span>OTA</span></button>
<button type="button" role="tab" data-tab="logs" aria-selected="false"><span class="tab-icon">≋</span><span>Nhật ký</span></button>
</nav>
<section class="card tab-panel overview-panel" data-panel="overview" role="tabpanel" aria-labelledby="connectionTitle">
<div class="overview-top">
  <div class="card-head"><div class="card-icon" aria-hidden="true">◉</div><div><h2 id="connectionTitle">Tổng quan hệ thống</h2><p class="hint">Trạng thái thiết bị ChatAI TPIOS</p></div></div>
  <button id="refreshDashboard" class="secondary overview-refresh" type="button">↻ Làm mới</button>
</div>
<div class="overview-hero">
  <div><span class="overview-eyebrow">TRẠNG THÁI THIẾT BỊ</span><strong id="overviewDeviceState">Đang kiểm tra...</strong><p id="deviceStatus" role="status" aria-live="polite">Đang kiểm tra Wi-Fi, Internet và Xiaozhi...</p></div>
  <span class="overview-chip" id="overviewBuildChip">Firmware —</span>
</div>
<div class="overview-grid">
  <section class="overview-group">
    <div class="overview-group-title"><span class="overview-group-icon">⌁</span><h3>Kết nối mạng</h3></div>
    <div class="overview-items">
      <div class="overview-item"><span>Wi-Fi / SSID</span><strong id="dashSsid">Đang tải...</strong></div>
      <div class="overview-item"><span>Tín hiệu</span><strong id="dashRssi">—</strong></div>
      <div class="overview-item"><span>Địa chỉ IP</span><strong id="dashIp">—</strong></div>
      <div class="overview-item"><span>Gateway / DNS</span><strong id="dashGateway">—</strong></div>
      <div class="overview-item"><span>Internet</span><strong id="dashInternet">—</strong></div>
      <div class="overview-item"><span>Kiểm tra DNS</span><strong id="dashDnsProbe">—</strong></div>
      <div class="overview-item"><span>Kết nối TCP 443</span><strong id="dashTcpProbe">—</strong></div>
      <div class="overview-item"><span>SoftAP</span><strong id="dashAp">—</strong></div>
    </div>
  </section>
  <section class="overview-group">
    <div class="overview-group-title"><span class="overview-group-icon">ᛒ</span><h3>Bluetooth LE</h3></div>
    <div class="overview-items">
      <div class="overview-item"><span>Trạng thái BLE</span><strong id="dashBleState">Đang kiểm tra...</strong></div>
      <div class="overview-item"><span>Thiết bị phát hiện</span><strong id="dashBleCount">—</strong></div>
    </div>
    <p class="overview-note">Mở mục Bluetooth để quét và xem chi tiết thiết bị.</p>
  </section>
  <section class="overview-group">
    <div class="overview-group-title"><span class="overview-group-icon">♫</span><h3>AI Voice · Xiaozhi</h3></div>
    <div class="overview-items">
      <div class="overview-item"><span>Máy chủ Xiaozhi</span><strong id="dashXiaozhi">—</strong></div>
      <div class="overview-item"><span>Microphone</span><strong id="dashMicState">—</strong></div>
      <div class="overview-item"><span>Loa</span><strong id="dashSpeakerState">—</strong></div>
      <div class="overview-item"><span>Âm lượng</span><strong id="dashVolume">—</strong></div>
    </div>
    <p class="overview-note">Gemini Live hiện đang tạm đóng; backend đang sử dụng là Xiaozhi.</p>
  </section>
  <section class="overview-group">
    <div class="overview-group-title"><span class="overview-group-icon">▦</span><h3>Tài nguyên & thiết bị</h3></div>
    <div class="overview-items">
      <div class="overview-item"><span>Heap khả dụng</span><strong id="dashHeap">—</strong></div>
      <div class="overview-item"><span>Heap thấp nhất</span><strong id="dashMinHeap">—</strong></div>
      <div class="overview-item"><span>Thời gian hoạt động</span><strong id="dashUptime">—</strong></div>
      <div class="overview-item"><span>Firmware</span><strong id="dashBuild">—</strong></div>
    </div>
  </section>
</div>
<div class="overview-footer">
  <span id="overviewUpdated">Chưa cập nhật trạng thái</span>
  <button id="restartDevice" class="danger" type="button">↻ Khởi động lại ESP32</button>
</div>
<p id="status" hidden></p>
<button id="retryGemini" hidden type="button">Thử kết nối lại Gemini</button>
<p id="restartResult" class="hint" role="status" aria-live="polite"></p>
</section>
<section class="card tab-panel" data-panel="wifi" role="tabpanel" aria-labelledby="wifiTitle" hidden>
<div class="card-head"><div class="card-icon" aria-hidden="true">⌁</div><div><h2 id="wifiTitle">Cấu hình Wi-Fi</h2><p class="hint">Quét mạng, đổi Wi-Fi và xác minh Internet trước khi lưu</p></div></div>
<p id="wifiAdminStatus" class="hint" role="status" aria-live="polite">Đang đọc trạng thái Wi-Fi...</p>
<label for="wifiSsid">Tên mạng Wi-Fi (SSID)</label>
<select id="wifiSsid" style="width:100%;padding:13px;border:1px solid #ffffff20;border-radius:12px;background:#0d0c19;color:#f5f1ff;font:inherit"><option value="">Chạm Quét Wi-Fi để tìm mạng</option></select>
<div class="actions"><button type="button" id="scanAdminWifi" class="secondary">Quét Wi-Fi</button></div>
<label for="wifiPassword">Mật khẩu Wi-Fi</label>
<div style="display:flex;gap:8px"><input id="wifiPassword" type="password" maxlength="64" autocomplete="current-password" placeholder="Nhập mật khẩu Wi-Fi" style="flex:1;min-width:0"><button type="button" id="toggleWifiPassword" class="secondary">Hiện</button></div>
<div class="actions"><button type="button" id="connectAdminWifi">Lưu và kết nối</button></div>
<p class="hint">Thông tin Wi-Fi chỉ được lưu vào NVS sau khi thiết bị kết nối router, nhận IP và xác minh DNS/TCP 443 thành công. SoftAP cấu hình được giữ trong lúc thử kết nối.</p>
</section>
<section class="card tab-panel" data-panel="bluetooth" role="tabpanel" aria-labelledby="bluetoothTitle" hidden>
<div class="card-head"><div class="card-icon" aria-hidden="true">ᛒ</div><div><h2 id="bluetoothTitle">Bluetooth Tool · BLE Scanner</h2><p class="hint">Quét thụ động các thiết bị Bluetooth Low Energy xung quanh</p></div></div>
<p id="bluetoothStatus" class="hint" role="status" aria-live="polite">Bluetooth chưa được khởi tạo. Bấm Bắt đầu quét để bật BLE Scanner.</p>
<div class="metric-grid">
<div class="metric"><span>Trạng thái BLE</span><strong id="bleState">Chưa khởi tạo</strong></div>
<div class="metric"><span>Thiết bị phát hiện</span><strong id="bleCount">0</strong></div>
</div>
<div class="actions"><button type="button" id="bleStart">▶ Bắt đầu quét</button><button type="button" id="bleStop" class="secondary" disabled>■ Dừng quét</button><button type="button" id="bleRefresh" class="secondary">↻ Làm mới</button></div>
<div class="ble-filter-row">
<input id="bleSearch" type="text" maxlength="48" placeholder="Tìm tên hoặc địa chỉ BLE" aria-label="Tìm thiết bị BLE">
<select id="bleSort" aria-label="Sắp xếp thiết bị"><option value="rssi">Tín hiệu mạnh</option><option value="name">Tên thiết bị</option><option value="recent">Mới phát hiện</option></select>
</div>
<div id="bleDevices" class="ble-device-list" aria-live="polite"><p class="hint">Chưa có dữ liệu quét.</p></div>
<p class="hint">Scanner dùng chế độ passive trong tối đa 10 giây mỗi lượt. Địa chỉ BLE riêng tư có thể thay đổi; RSSI chỉ phản ánh cường độ tín hiệu, không phải khoảng cách chính xác. Khi đang quét, Wi-Fi và âm thanh có thể chịu ảnh hưởng do dùng chung radio 2.4 GHz.</p>
</section>
<section class="card tab-panel" data-panel="logs" role="tabpanel" aria-labelledby="diagnosticsTitle" hidden>
<div class="card-head"><div class="card-icon" aria-hidden="true">≋</div><div><h2 id="diagnosticsTitle">Log chẩn đoán ChatAI / Xiaozhi</h2><p class="hint">Tối đa 8 KB · Không lưu API Key hoặc dữ liệu âm thanh</p></div></div>
<pre id="diagnosticLog" style="white-space:pre-wrap;overflow-wrap:anywhere;max-height:260px;overflow:auto;background:#0d0c19;border:1px solid #ffffff18;border-radius:12px;padding:12px;color:#d9d2f0;font:11px/1.55 ui-monospace,monospace">Đang tải log...</pre>
<textarea id="diagnosticCopyFallback" readonly hidden aria-label="Log chẩn đoán để sao chép" style="width:100%;min-height:150px;margin-top:10px;padding:12px;border:1px solid #a477ff;border-radius:12px;background:#0d0c19;color:#f5f1ff;font:11px/1.55 ui-monospace,monospace"></textarea>
<div class="actions"><button id="copyDiagnostic" class="secondary">Sao chép log</button><button id="downloadDiagnostic" class="secondary">Tải log TXT</button><button id="clearDiagnostic" class="secondary">Xóa log</button></div>
<p id="diagnosticResult" class="hint" role="status" aria-live="polite"></p>
</section>
<section class="card tab-panel" data-panel="gemini" role="tabpanel" aria-labelledby="geminiTitle" hidden>
<div class="card-head"><div class="card-icon" aria-hidden="true">✧</div><div><h2 id="geminiTitle">Gemini Live (đang tắt)</h2><p class="hint">Firmware hiện chỉ sử dụng server Xiaozhi. Các thiết lập Gemini được giữ lại nhưng không được sử dụng.</p></div></div>
<label for="apiKey">Gemini API Key</label>
<input id="apiKey" type="password" maxlength="256" autocomplete="new-password" autocapitalize="off" spellcheck="false" placeholder="Nhập API Key mới">
<p class="hint">Khóa đã lưu không được gửi ngược về trình duyệt. Để giữ khóa hiện tại, hãy để trống và không nhấn lưu.</p>
<div class="actions"><button id="save">Lưu API Key</button><button id="remove" class="secondary" hidden>Xóa API Key</button></div>
<p id="result" role="status" aria-live="polite"></p>
<hr>
<h2>Giọng nói Gemini Live</h2>
<p class="hint">Giọng được áp dụng khi tạo phiên Gemini Live tiếp theo. API Live hiện không có endpoint nghe thử riêng; không phát thử bằng cách chen âm thanh vào cuộc hội thoại đang chạy.</p>
<label for="geminiVoice">Chọn giọng</label>
<select id="geminiVoice">
<option>Achernar</option><option>Achird</option><option>Algenib</option><option>Algieba</option><option>Alnilam</option><option>Aoede</option><option>Autonoe</option><option>Callirrhoe</option><option>Charon</option><option>Despina</option><option>Enceladus</option><option>Erinome</option><option>Fenrir</option><option>Gacrux</option><option>Iapetus</option><option>Kore</option><option>Laomedeia</option><option>Leda</option><option>Orus</option><option>Puck</option><option>Pulcherrima</option><option>Rasalgethi</option><option>Sadachbia</option><option>Sadaltager</option><option>Schedar</option><option>Sulafat</option><option>Umbriel</option><option>Vindemiatrix</option><option>Zephyr</option><option>Zubenelgenubi</option>
</select>
<div class="actions"><button id="saveVoice">Lưu giọng nói</button></div><p id="voiceResult" class="hint" role="status" aria-live="polite"></p>
<hr>
<h2>Vai trò và tính cách AI</h2>
<p class="hint">Chọn mẫu để điền nhanh, sau đó có thể sửa nội dung theo ý anh.</p>
<label for="promptTemplate">Mẫu System Prompt</label>
<select id="promptTemplate">
<option value="custom">Tùy chỉnh</option><option value="personal">Trợ lý cá nhân</option><option value="companion">Bạn gái AI thân thiện</option><option value="tech">Chuyên gia công nghệ</option><option value="coding">Trợ lý lập trình</option><option value="language">Giáo viên ngoại ngữ</option><option value="support">Chăm sóc khách hàng</option>
</select>
<label for="systemPrompt">System Prompt</label>
<textarea id="systemPrompt" placeholder="Mô tả vai trò, tính cách, ngôn ngữ và quy tắc phản hồi của AI..."></textarea>
<p class="hint"><span id="promptBytes">0</span> byte UTF-8 · tối đa <span id="promptMaxBytes">0</span> byte theo bộ nhớ thiết bị.</p>
<div class="actions"><button id="savePrompt">Lưu System Prompt</button><button id="resetPrompt" class="secondary">Khôi phục mặc định</button></div>
<p id="promptResult" class="hint" role="status" aria-live="polite"></p>
</section>
<section class="card tab-panel" data-panel="audio" role="tabpanel" aria-labelledby="audioTitle" hidden>
<div class="card-head"><div class="card-icon" aria-hidden="true">♫</div><div><h2 id="audioTitle">Âm thanh</h2><p class="hint">Điều chỉnh âm lượng loa Xiaozhi</p></div></div>
<label for="speakerVolume">Âm lượng loa: <output id="speakerVolumeValue">100%</output></label>
<input id="speakerVolume" type="range" min="0" max="100" step="1" value="100" style="width:100%;accent-color:#9b5cff">
<p class="hint">Thiết lập được lưu riêng vào NVS và tự áp dụng sau khi lưu.</p>
<div class="actions"><button id="saveSpeakerVolume">Lưu âm lượng</button></div>
<p id="audioResult" class="hint" role="status" aria-live="polite"></p>
<hr>
<h2>Độ nhạy Microphone</h2>
<p class="hint">Điều chỉnh gain số từ -6 đến +6 dB trước AFE/AGC. Nếu tín hiệu đã chạm 0 dBFS, hãy giảm gain; theo dõi log MIC_LEVEL để kiểm tra clipping.</p>
<label for="micGain">Độ nhạy microphone: <output id="micGainValue">0 dB</output></label>
<input id="micGain" type="range" min="-6" max="6" step="1" value="0" style="width:100%;accent-color:#55d6c2">
<div class="actions"><button id="saveMicGain">Lưu độ nhạy</button><button id="resetMicGain" class="secondary">Khôi phục mặc định</button></div>
<p id="micGainResult" class="hint" role="status" aria-live="polite"></p>
</section>
<section class="card tab-panel" data-panel="ota" role="tabpanel" aria-labelledby="otaTitle" hidden>
<div class="card-head"><div class="card-icon" aria-hidden="true">⇧</div><div><h2 id="otaTitle">Cập nhật Firmware OTA</h2><p class="hint">Cập nhật trực tiếp từ điện thoại, không cần cáp USB</p></div></div>
<p id="firmwareStatus" role="status">Đang đọc phiên bản firmware...</p>
<p class="hint">Chọn tệp BIN hoặc ZIP chỉ chứa một firmware ứng dụng BIN. Không tắt nguồn trong quá trình cập nhật.</p>
<label for="firmware">Tệp firmware</label>
<input id="firmware" type="file" accept=".bin,.zip,application/octet-stream,application/zip">
<p id="otaFileInfo" class="muted">Chưa chọn tệp.</p>
<button id="updateFirmware">Kiểm tra và cài đặt</button>
<div class="progress" aria-label="Tiến trình tải firmware"><div id="otaProgress"></div></div>
<p id="otaResult" role="status" aria-live="polite"></p>
</section>
<p class="foot">ChatAI TPIOS · Web Admin nội bộ</p>
</main>
<script>
const csrfToken = "__CSRF_TOKEN__";
const statusText = document.getElementById("status");
const deviceStatus = document.getElementById("deviceStatus");
const retryGeminiButton = document.getElementById("retryGemini");
const restartDeviceButton = document.getElementById("restartDevice");
const restartResult = document.getElementById("restartResult");
const resultText = document.getElementById("result");
const keyInput = document.getElementById("apiKey");
const saveButton = document.getElementById("save");
const removeButton = document.getElementById("remove");
const firmwareInput = document.getElementById("firmware");
const updateFirmwareButton = document.getElementById("updateFirmware");
const otaResult = document.getElementById("otaResult");
const otaFileInfo = document.getElementById("otaFileInfo");
const otaProgress = document.getElementById("otaProgress");
const firmwareStatus = document.getElementById("firmwareStatus");
const diagnosticLog = document.getElementById("diagnosticLog");
const diagnosticResult = document.getElementById("diagnosticResult");
const copyDiagnosticButton = document.getElementById("copyDiagnostic");
const downloadDiagnosticButton = document.getElementById("downloadDiagnostic");
const clearDiagnosticButton = document.getElementById("clearDiagnostic");
const speakerVolumeInput = document.getElementById("speakerVolume");
const speakerVolumeValue = document.getElementById("speakerVolumeValue");
const saveSpeakerVolumeButton = document.getElementById("saveSpeakerVolume");
const audioResult = document.getElementById("audioResult");
const micGainInput = document.getElementById("micGain");
const micGainValue = document.getElementById("micGainValue");
const saveMicGainButton = document.getElementById("saveMicGain");
const resetMicGainButton = document.getElementById("resetMicGain");
const micGainResult = document.getElementById("micGainResult");
let speakerVolumeDirty = false;
let speakerVolumeSaving = false;
let micGainDirty = false;
let micGainSaving = false;
const voiceSelect = document.getElementById("geminiVoice");
const voiceResult = document.getElementById("voiceResult");
const promptInput = document.getElementById("systemPrompt");
const promptTemplate = document.getElementById("promptTemplate");
const promptBytes = document.getElementById("promptBytes");
const promptMaxBytesLabel = document.getElementById("promptMaxBytes");
const promptResult = document.getElementById("promptResult");
let maxPromptBytes = 0;
const wifiSsidInput = document.getElementById("wifiSsid");
const wifiPasswordInput = document.getElementById("wifiPassword");
const wifiAdminStatus = document.getElementById("wifiAdminStatus");
const scanAdminWifiButton = document.getElementById("scanAdminWifi");
const connectAdminWifiButton = document.getElementById("connectAdminWifi");
const toggleWifiPasswordButton = document.getElementById("toggleWifiPassword");
let wifiPollAttempts = 0;
const bluetoothStatus = document.getElementById("bluetoothStatus");
const bleState = document.getElementById("bleState");
const bleCount = document.getElementById("bleCount");
const bleStartButton = document.getElementById("bleStart");
const bleStopButton = document.getElementById("bleStop");
const bleRefreshButton = document.getElementById("bleRefresh");
const bleSearchInput = document.getElementById("bleSearch");
const bleSortSelect = document.getElementById("bleSort");
const bleDevicesContainer = document.getElementById("bleDevices");
let bleCachedDevices = [];
let blePollTimer = null;
let otaMaxBytes = 0;
let currentBuildNumber = 0;

document.querySelectorAll("[data-tab]").forEach(button => {
  button.addEventListener("click", () => {
    const selected = button.dataset.tab;
    document.querySelectorAll("[data-tab]").forEach(tab => {
      tab.setAttribute("aria-selected", String(tab === button));
    });
    document.querySelectorAll("[data-panel]").forEach(panel => {
      panel.hidden = panel.dataset.panel !== selected;
    });
    if (selected === "bluetooth") refreshBluetooth();
    if (selected === "overview") { refreshDeviceStatus().catch(error => { deviceStatus.textContent = error.message; }); refreshBluetoothOverview(); }
  });
});

speakerVolumeInput.addEventListener("input", () => {
  speakerVolumeDirty = true;
  speakerVolumeValue.textContent = speakerVolumeInput.value + "%";
});
saveSpeakerVolumeButton.addEventListener("click", async () => {
  const requestedVolume = Number(speakerVolumeInput.value);
  speakerVolumeSaving = true;
  saveSpeakerVolumeButton.disabled = true;
  audioResult.textContent = "Đang lưu âm lượng...";
  try {
    const response = await fetch("/api/audio/volume", {
      method:"POST", cache:"no-store",
      headers:{"Content-Type":"application/json","X-ChatAI-CSRF":csrfToken},
      body:JSON.stringify({volume:requestedVolume})
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(data.error || ("Không lưu được âm lượng (HTTP " + response.status + ")"));
    speakerVolumeInput.value = String(requestedVolume);
    speakerVolumeValue.textContent = requestedVolume + "%";
    speakerVolumeDirty = false;
    audioResult.textContent = data.message || "Đã lưu âm lượng loa.";
  } catch (error) {
    audioResult.textContent = error.message;
  } finally { speakerVolumeSaving = false; saveSpeakerVolumeButton.disabled = false; }
});
micGainInput.addEventListener("input", () => {
  micGainDirty = true;
  micGainValue.textContent = micGainInput.value + " dB";
});
async function saveMicGain() {
  const requestedGain = Number(micGainInput.value);
  micGainSaving = true;
  saveMicGainButton.disabled = true;
  resetMicGainButton.disabled = true;
  micGainResult.textContent = "Đang lưu độ nhạy microphone...";
  try {
    const response = await fetch("/api/audio/mic-gain", {
      method:"POST", cache:"no-store",
      headers:{"Content-Type":"application/json","X-ChatAI-CSRF":csrfToken},
      body:JSON.stringify({gain_db:requestedGain})
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(data.error || ("Không lưu được độ nhạy (HTTP " + response.status + ")"));
    micGainDirty = false;
    micGainResult.textContent = data.message || "Đã lưu độ nhạy microphone.";
  } catch (error) {
    micGainResult.textContent = error.message;
  } finally {
    micGainSaving = false;
    saveMicGainButton.disabled = false;
    resetMicGainButton.disabled = false;
  }
}
saveMicGainButton.addEventListener("click", saveMicGain);
resetMicGainButton.addEventListener("click", () => {
  micGainInput.value = "0";
  micGainValue.textContent = "0 dB";
  micGainDirty = true;
  saveMicGain();
});

async function scanAdminWifi() {
  scanAdminWifiButton.disabled = true;
  wifiAdminStatus.textContent = "Đang quét các mạng Wi-Fi...";
  try {
    const response = await fetch("/api/wifi/scan", {cache:"no-store"});
    const data = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(data.error || ("Quét Wi-Fi thất bại (HTTP " + response.status + ")"));
    const previous = wifiSsidInput.value;
    wifiSsidInput.replaceChildren();
    (data.networks || []).forEach(network => {
      const option = document.createElement("option");
      option.value = network.ssid;
      option.textContent = network.ssid + " · " + network.rssi + " dBm";
      wifiSsidInput.appendChild(option);
    });
    if (!wifiSsidInput.options.length) {
      const option = document.createElement("option");
      option.value = "";
      option.textContent = "Không tìm thấy mạng Wi-Fi";
      wifiSsidInput.appendChild(option);
    } else if (Array.from(wifiSsidInput.options).some(option => option.value === previous)) {
      wifiSsidInput.value = previous;
    }
    wifiAdminStatus.textContent = "Đã tìm thấy " + (data.networks || []).length + " mạng Wi-Fi.";
  } catch (error) {
    wifiAdminStatus.textContent = error.message;
  } finally { scanAdminWifiButton.disabled = false; }
}
scanAdminWifiButton.addEventListener("click", scanAdminWifi);
function bleSignalLabel(rssi) {
  if (rssi >= -55) return "Rất mạnh";
  if (rssi >= -65) return "Mạnh";
  if (rssi >= -75) return "Trung bình";
  if (rssi >= -88) return "Yếu";
  return "Rất yếu";
}
function renderBluetoothDevices() {
  const query = bleSearchInput.value.trim().toLowerCase();
  const devices = bleCachedDevices.filter(device =>
    (device.name || "").toLowerCase().includes(query) ||
    (device.address || "").toLowerCase().includes(query));
  if (bleSortSelect.value === "name") {
    devices.sort((a, b) => (a.name || "").localeCompare(b.name || ""));
  } else if (bleSortSelect.value === "recent") {
    devices.sort((a, b) => Number(a.last_seen_seconds || 0) - Number(b.last_seen_seconds || 0));
  } else {
    devices.sort((a, b) => Number(b.rssi) - Number(a.rssi));
  }
  bleDevicesContainer.replaceChildren();
  if (!devices.length) {
    const empty = document.createElement("p");
    empty.className = "hint";
    empty.textContent = query ? "Không tìm thấy thiết bị phù hợp." : "Chưa phát hiện thiết bị BLE. Hãy bắt đầu quét.";
    bleDevicesContainer.appendChild(empty);
    return;
  }
  devices.forEach(device => {
    const card = document.createElement("article");
    card.className = "ble-device";
    const head = document.createElement("div");
    head.className = "ble-device-head";
    const identity = document.createElement("div");
    identity.style.minWidth = "0";
    const name = document.createElement("div");
    name.className = "ble-device-name";
    name.textContent = device.name || "Thiết bị không công bố tên";
    const address = document.createElement("div");
    address.className = "ble-device-address";
    address.textContent = device.address || "Không có địa chỉ";
    identity.append(name, address);
    const rssi = Number(device.rssi);
    const signal = document.createElement("span");
    signal.className = "ble-rssi";
    signal.textContent = rssi + " dBm";
    head.append(identity, signal);
    card.appendChild(head);
    const track = document.createElement("div");
    track.className = "ble-rssi-track";
    const fill = document.createElement("div");
    fill.className = "ble-rssi-fill";
    fill.style.width = Math.max(0, Math.min(100, (rssi + 100) * 2)) + "%";
    track.appendChild(fill);
    card.appendChild(track);
    const meta = document.createElement("div");
    meta.className = "ble-device-meta";
    const details = [
      ["Tín hiệu", bleSignalLabel(rssi)],
      ["Loại địa chỉ", device.address_type || "Không rõ"],
      ["Phát hiện gần nhất", Number(device.last_seen_seconds || 0) + " giây trước"],
      ["Manufacturer Data", device.manufacturer_data || "Không quảng bá"],
      ["Service UUID", device.service_uuids || "Không quảng bá"],
      ["Advertising Data", device.advertising_data || "Không có"]
    ];
    details.forEach(([label, value]) => {
      const line = document.createElement("div");
      const strong = document.createElement("strong");
      strong.textContent = label + ": ";
      line.append(strong, document.createTextNode(value));
      meta.appendChild(line);
    });
    card.appendChild(meta);
    bleDevicesContainer.appendChild(card);
  });
}
function scheduleBluetoothPoll() {
  if (blePollTimer !== null) return;
  blePollTimer = setTimeout(async () => {
    blePollTimer = null;
    await refreshBluetooth();
  }, 1500);
}
async function refreshBluetooth() {
  try {
    const statusResponse = await fetch("/api/bluetooth/status", {cache:"no-store"});
    const state = await statusResponse.json().catch(() => ({}));
    if (!statusResponse.ok) throw new Error(state.error || ("Không đọc được trạng thái Bluetooth (HTTP " + statusResponse.status + ")"));
    bleState.textContent = state.error_code ? ("Lỗi (" + state.error_code + ")") :
      state.scanning ? "Đang quét" : state.scan_requested ? "Đang chờ khởi tạo" :
      state.ready ? "Sẵn sàng" : state.initialized ? "Đang khởi tạo" : "Chưa khởi tạo";
    bleCount.textContent = String(state.device_count || 0);
    bluetoothStatus.textContent = state.message || "Trạng thái Bluetooth đã cập nhật.";
    bleStartButton.disabled = Boolean(state.scanning);
    bleStopButton.disabled = !state.scanning;
    const devicesResponse = await fetch("/api/bluetooth/devices", {cache:"no-store"});
    const data = await devicesResponse.json().catch(() => ({}));
    if (!devicesResponse.ok) throw new Error(data.error || ("Không đọc được danh sách BLE (HTTP " + devicesResponse.status + ")"));
    bleCachedDevices = Array.isArray(data.devices) ? data.devices : [];
    renderBluetoothDevices();
    if (state.scanning || state.scan_requested) scheduleBluetoothPoll();
  } catch (error) {
    bluetoothStatus.textContent = error.message;
  }
}
async function sendBluetoothCommand(path, button, fallbackMessage) {
  button.disabled = true;
  bluetoothStatus.textContent = "Đang gửi yêu cầu đến BLE Scanner...";
  try {
    const response = await fetch(path, {
      method:"POST", cache:"no-store",
      headers:{"X-ChatAI-CSRF":csrfToken}
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(data.error || ("Yêu cầu Bluetooth thất bại (HTTP " + response.status + ")"));
    bluetoothStatus.textContent = data.message || fallbackMessage;
    await refreshBluetooth();
  } catch (error) {
    bluetoothStatus.textContent = "Không gửi được lệnh đến ESP32 (" + path + "): " + error.message;
    button.disabled = false;
  }
}
bleStartButton.addEventListener("click", () => sendBluetoothCommand("/api/bluetooth/scan/start", bleStartButton, "Đã yêu cầu bắt đầu quét BLE."));
bleStopButton.addEventListener("click", () => sendBluetoothCommand("/api/bluetooth/scan/stop", bleStopButton, "Đã yêu cầu dừng quét BLE."));
bleRefreshButton.addEventListener("click", refreshBluetooth);
bleSearchInput.addEventListener("input", renderBluetoothDevices);
bleSortSelect.addEventListener("change", renderBluetoothDevices);
toggleWifiPasswordButton.addEventListener("click", () => {
  const visible = wifiPasswordInput.type === "password";
  wifiPasswordInput.type = visible ? "text" : "password";
  toggleWifiPasswordButton.textContent = visible ? "Ẩn" : "Hiện";
});
async function pollAdminWifiStatus() {
  try {
    const response = await fetch("/api/wifi/status", {cache:"no-store"});
    if (!response.ok) throw new Error("Không đọc được trạng thái Wi-Fi (HTTP " + response.status + ")");
    const state = await response.json();
    wifiAdminStatus.textContent = state.message + (state.ip ? " IP: " + state.ip : "");
    if ((state.state === 1 || state.state === 2 || state.state === 5) && wifiPollAttempts++ < 60) {
      setTimeout(pollAdminWifiStatus, 1500);
    } else if (state.state === 3) {
      wifiPasswordInput.value = "";
      await refreshDeviceStatus();
      wifiAdminStatus.textContent = state.message + (state.ip ? " IP: " + state.ip : "");
    }
  } catch (error) { wifiAdminStatus.textContent = error.message; }
}
connectAdminWifiButton.addEventListener("click", async () => {
  const ssid = wifiSsidInput.value;
  const password = wifiPasswordInput.value;
  if (!ssid) { wifiAdminStatus.textContent = "Anh hãy chọn mạng Wi-Fi trước."; return; }
  if (password.length > 64) { wifiAdminStatus.textContent = "Mật khẩu Wi-Fi vượt quá 64 ký tự."; return; }
  connectAdminWifiButton.disabled = true;
  wifiPollAttempts = 0;
  wifiAdminStatus.textContent = "Đang gửi cấu hình Wi-Fi...";
  try {
    const response = await fetch("/api/wifi/connect", {
      method:"POST", cache:"no-store",
      headers:{"Content-Type":"application/json","X-ChatAI-CSRF":csrfToken},
      body:JSON.stringify({ssid,password})
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(data.error || ("Không gửi được cấu hình Wi-Fi (HTTP " + response.status + ")"));
    wifiAdminStatus.textContent = data.message || "Đã nhận cấu hình. Đang kết nối router...";
    pollAdminWifiStatus();
  } catch (error) { wifiAdminStatus.textContent = error.message; }
  finally { connectAdminWifiButton.disabled = false; }
});

function formatBytes(bytes) {
  return (bytes / 1048576).toFixed(2) + " MB";
}

async function refreshDiagnostics() {
  const response = await fetch("/api/diagnostics", {cache:"no-store"});
  if (!response.ok) throw new Error("Không đọc được log (HTTP " + response.status + ")");
  diagnosticLog.textContent = await response.text() || "Chưa có log chẩn đoán.";
  diagnosticLog.scrollTop = diagnosticLog.scrollHeight;
}
const diagnosticCopyFallback = document.getElementById("diagnosticCopyFallback");

function tryLegacyDiagnosticCopy(text) {
  const temp = document.createElement("textarea");
  temp.value = text;
  temp.setAttribute("readonly", "");
  temp.style.position = "fixed";
  temp.style.top = "0";
  temp.style.left = "-9999px";
  temp.style.opacity = "0";
  document.body.appendChild(temp);
  try {
    temp.focus();
    temp.select();
    temp.setSelectionRange(0, temp.value.length);
    return document.execCommand("copy");
  } catch (_) {
    return false;
  } finally {
    temp.remove();
  }
}

function showDiagnosticCopyFallback(text) {
  diagnosticCopyFallback.value = text;
  diagnosticCopyFallback.hidden = false;
  diagnosticCopyFallback.focus();
  diagnosticCopyFallback.select();
  diagnosticCopyFallback.setSelectionRange(0, text.length);
  diagnosticResult.textContent =
    "Safari đã chặn sao chép tự động. Log đã được chọn trong ô bên dưới; anh có thể chạm giữ để sao chép.";
}

copyDiagnosticButton.addEventListener("click", () => {
  const text = diagnosticLog.textContent || "";
  diagnosticCopyFallback.hidden = true;

  // Start Clipboard API synchronously in the click handler; do not await a log refresh first.
  if (navigator.clipboard && typeof navigator.clipboard.writeText === "function") {
    try {
      navigator.clipboard.writeText(text).then(() => {
        diagnosticResult.textContent = "Đã sao chép log đang hiển thị.";
      }).catch(() => {
        if (tryLegacyDiagnosticCopy(text)) {
          diagnosticResult.textContent = "Đã sao chép log.";
        } else {
          showDiagnosticCopyFallback(text);
        }
      });
      return;
    } catch (_) {
      // Continue to the synchronous legacy fallback below.
    }
  }

  if (tryLegacyDiagnosticCopy(text)) {
    diagnosticResult.textContent = "Đã sao chép log.";
  } else {
    showDiagnosticCopyFallback(text);
  }
});
downloadDiagnosticButton.addEventListener("click", () => {
  // A direct same-origin navigation preserves the user gesture on mobile Safari.
  window.location.href = "/api/diagnostics/download";
});
clearDiagnosticButton.addEventListener("click", async () => {
  try {
    const response = await fetch("/api/diagnostics/clear", {method:"POST", cache:"no-store", headers:{"X-ChatAI-CSRF":csrfToken}});
    if (!response.ok) throw new Error("HTTP " + response.status);
    await refreshDiagnostics();
    diagnosticResult.textContent = "Đã xóa log chẩn đoán.";
  } catch (error) { diagnosticResult.textContent = "Không xóa được log: " + error.message; }
});
async function refreshFirmwareStatus() {
  const response = await fetch("/api/firmware/status", {cache:"no-store"});
  if (!response.ok) throw new Error("Không đọc được phiên bản firmware (HTTP " + response.status + ")");
  const data = await response.json();
  currentBuildNumber = Number(data.build_number || 0);
  otaMaxBytes = Number(data.ota_max_bytes || 0);
  firmwareStatus.textContent = "Bản dựng firmware: #" + currentBuildNumber +
    (data.installed_firmware ? " · Đã cài: " + data.installed_firmware : " · Chưa ghi nhận tên tệp") +
    " · Phân vùng đang chạy: " + (data.running_partition || "không rõ") +
    " · Phân vùng OTA: " + (data.ota_target || "không khả dụng");
}

function zipCrc32(bytes) {
  let crc = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i];
    for (let bit = 0; bit < 8; bit++) crc = (crc & 1) ? (crc >>> 1) ^ 0xEDB88320 : crc >>> 1;
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

async function readFirmwareBuild(blob) {
  const bytes = new Uint8Array(await blob.arrayBuffer());
  if (bytes.length < 64 || bytes[0] !== 0xE9 ||
      bytes[32] !== 0x32 || bytes[33] !== 0x54 ||
      bytes[34] !== 0xCD || bytes[35] !== 0xAB) {
    throw new Error("Tệp không có cấu trúc ứng dụng ESP-IDF hợp lệ.");
  }
  const marker = new TextEncoder().encode("CHATAI_BUILD_NUMBER=");
  for (let i = 0; i <= bytes.length - marker.length; i++) {
    let match = true;
    for (let j = 0; j < marker.length; j++) {
      if (bytes[i + j] !== marker[j]) { match = false; break; }
    }
    if (!match) continue;
    let end = i + marker.length;
    while (end < bytes.length && bytes[end] >= 48 && bytes[end] <= 57) end++;
    if (end === i + marker.length) continue;
    const value = Number(new TextDecoder().decode(bytes.slice(i + marker.length, end)));
    if (Number.isSafeInteger(value) && value >= 0) return value;
  }
  throw new Error("Không tìm thấy số bản dựng trong firmware. Hãy chọn BIN chính thức của ChatAI TPIOS.");
}

async function extractOtaBinFromZip(file) {
  if (typeof DecompressionStream === "undefined") {
    throw new Error("Trình duyệt không hỗ trợ giải nén ZIP. Hãy cập nhật Safari hoặc chọn trực tiếp tệp BIN.");
  }
  const maxZipBytes = Math.min(16 * 1024 * 1024, Math.max(otaMaxBytes + 1048576, otaMaxBytes * 2));
  if (file.size > maxZipBytes) throw new Error("Tệp ZIP vượt giới hạn an toàn (" + formatBytes(maxZipBytes) + ").");

  const tailSize = Math.min(file.size, 22 + 65535);
  const tailStart = file.size - tailSize;
  const tail = new DataView(await file.slice(tailStart).arrayBuffer());
  let eocd = -1;
  for (let i = tail.byteLength - 22; i >= 0; i--) {
    if (tail.getUint32(i, true) === 0x06054B50 &&
        i + 22 + tail.getUint16(i + 20, true) === tail.byteLength) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error("Tệp ZIP không hợp lệ: không tìm thấy thư mục trung tâm.");

  const disk = tail.getUint16(eocd + 4, true);
  const centralDisk = tail.getUint16(eocd + 6, true);
  const diskEntries = tail.getUint16(eocd + 8, true);
  const totalEntries = tail.getUint16(eocd + 10, true);
  const centralSize = tail.getUint32(eocd + 12, true);
  const centralOffset = tail.getUint32(eocd + 16, true);
  const eocdOffset = tailStart + eocd;
  if (disk || centralDisk || diskEntries !== totalEntries || totalEntries === 0xFFFF ||
      centralSize === 0xFFFFFFFF || centralOffset === 0xFFFFFFFF || totalEntries > 128 ||
      centralOffset + centralSize > eocdOffset) {
    throw new Error("Tệp ZIP nhiều phân vùng, dùng ZIP64 hoặc có cấu trúc vượt giới hạn hỗ trợ.");
  }

  const directory = new DataView(await file.slice(centralOffset, centralOffset + centralSize).arrayBuffer());
  const decoder = new TextDecoder("utf-8", {fatal:true});
  const binaries = [];
  let offset = 0;
  for (let i = 0; i < totalEntries; i++) {
    if (offset + 46 > directory.byteLength || directory.getUint32(offset, true) !== 0x02014B50) {
      throw new Error("Tệp ZIP bị lỗi: thư mục trung tâm không hợp lệ.");
    }
    const flags = directory.getUint16(offset + 8, true);
    const method = directory.getUint16(offset + 10, true);
    const crc = directory.getUint32(offset + 16, true);
    const compressedSize = directory.getUint32(offset + 20, true);
    const uncompressedSize = directory.getUint32(offset + 24, true);
    const nameLength = directory.getUint16(offset + 28, true);
    const extraLength = directory.getUint16(offset + 30, true);
    const commentLength = directory.getUint16(offset + 32, true);
    const startDisk = directory.getUint16(offset + 34, true);
    const localOffset = directory.getUint32(offset + 42, true);
    const end = offset + 46 + nameLength + extraLength + commentLength;
    if (end > directory.byteLength || startDisk !== 0 || compressedSize === 0xFFFFFFFF ||
        uncompressedSize === 0xFFFFFFFF || localOffset === 0xFFFFFFFF) {
      throw new Error("Tệp ZIP bị lỗi hoặc dùng ZIP64 không được hỗ trợ.");
    }
    let name;
    try {
      name = decoder.decode(new Uint8Array(directory.buffer, directory.byteOffset + offset + 46, nameLength));
    } catch (_) {
      throw new Error("Tên tệp trong ZIP không phải UTF-8.");
    }
    if (!name || name.includes("\\") || name.startsWith("/") || /^[A-Za-z]:/.test(name) ||
        name.split("/").some(part => part === "..") || name.includes("\0")) {
      throw new Error("Tệp ZIP chứa đường dẫn không an toàn.");
    }
    if (!name.endsWith("/") && /\.bin$/i.test(name)) {
      if ((flags & 0x2041) !== 0) throw new Error("Tệp ZIP chứa BIN được mã hóa hoặc có cờ không an toàn.");
      if (method !== 0 && method !== 8) throw new Error("Tệp ZIP dùng kiểu nén BIN không được hỗ trợ.");
      binaries.push({name, flags, method, crc, compressedSize, uncompressedSize, localOffset});
    }
    offset = end;
  }
  if (offset !== directory.byteLength) throw new Error("Tệp ZIP có dữ liệu thừa hoặc thư mục không hợp lệ.");

  const excluded = /(^|\/)(bootloader|boot_app0|partition-table|partitions|ota_data_initial|usb-flash|merged|flash|firmware-full)\.bin$/i;
  const candidates = binaries.filter(entry => !excluded.test(entry.name));
  if (candidates.length !== 1) {
    throw new Error(candidates.length
      ? "Tệp ZIP có nhiều BIN. Hãy dùng ZIP chỉ chứa một firmware ứng dụng."
      : "Không tìm thấy firmware ứng dụng BIN trong tệp ZIP.");
  }
  const entry = candidates[0];
  if (entry.uncompressedSize < 4096 || entry.uncompressedSize > otaMaxBytes ||
      entry.compressedSize === 0 || entry.compressedSize > maxZipBytes) {
    throw new Error("Kích thước firmware trong ZIP không hợp lệ hoặc vượt dung lượng OTA.");
  }

  const localHeader = new DataView(await file.slice(entry.localOffset, entry.localOffset + 30).arrayBuffer());
  if (localHeader.byteLength !== 30 || localHeader.getUint32(0, true) !== 0x04034B50 ||
      localHeader.getUint16(6, true) !== entry.flags || localHeader.getUint16(8, true) !== entry.method) {
    throw new Error("Phần đầu firmware trong ZIP không khớp với thư mục trung tâm.");
  }
  const localNameLength = localHeader.getUint16(26, true);
  const localExtraLength = localHeader.getUint16(28, true);
  const dataStart = entry.localOffset + 30 + localNameLength + localExtraLength;
  const dataEnd = dataStart + entry.compressedSize;
  if (dataStart < entry.localOffset || dataEnd > centralOffset || dataEnd > file.size) {
    throw new Error("Vùng dữ liệu firmware nằm ngoài giới hạn của tệp ZIP.");
  }
  const localName = decoder.decode(await file.slice(entry.localOffset + 30,
    entry.localOffset + 30 + localNameLength).arrayBuffer());
  if (localName !== entry.name) throw new Error("Tên firmware trong phần đầu ZIP không khớp.");

  otaResult.textContent = "Đang giải nén " + entry.name + "...";
  let firmware;
  if (entry.method === 0) {
    if (entry.compressedSize !== entry.uncompressedSize) throw new Error("Kích thước BIN trong ZIP không khớp.");
    firmware = file.slice(dataStart, dataEnd, "application/octet-stream");
  } else {
    try {
      firmware = await new Response(file.slice(dataStart, dataEnd).stream()
        .pipeThrough(new DecompressionStream("deflate-raw"))).blob();
    } catch (_) {
      throw new Error("Safari không giải nén được ZIP Deflate trên thiết bị này. Hãy chọn tệp BIN.");
    }
  }
  if (firmware.size !== entry.uncompressedSize || firmware.size > otaMaxBytes) {
    throw new Error("Giải nén thất bại: kích thước BIN không khớp hoặc vượt phân vùng OTA.");
  }
  if (zipCrc32(new Uint8Array(await firmware.arrayBuffer())) !== entry.crc) {
    throw new Error("CRC32 không khớp: tệp ZIP hoặc firmware đã bị hỏng.");
  }
  return {blob:firmware, name:entry.name.split("/").pop()};
}

function uploadOtaFirmware(firmware, filename) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/ota");
    xhr.setRequestHeader("X-ChatAI-CSRF", csrfToken);
    xhr.setRequestHeader("X-Firmware-Filename", filename);
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    xhr.upload.onprogress = event => {
      if (event.lengthComputable) {
        const percent = Math.round(event.loaded * 100 / event.total);
        otaProgress.style.width = percent + "%";
        otaResult.textContent = percent < 100
          ? "Đang tải lên " + filename + ": " + percent + "%"
          : "Đã tải lên 100%. ESP32 đang xác thực và ghi firmware vào phân vùng OTA...";
      }
    };
    xhr.onload = () => {
      let data = {};
      try { data = JSON.parse(xhr.responseText || "{}"); } catch (_) {}
      if (xhr.status >= 200 && xhr.status < 300) resolve(data);
      else reject(new Error(data.error || ("OTA thất bại (HTTP " + xhr.status + ")")));
    };
    xhr.onerror = () => reject(new Error("Mất kết nối khi tải firmware. Kiểm tra thiết bị trước khi thử lại."));
    xhr.send(firmware);
  });
}

firmwareInput.addEventListener("change", () => {
  const file = firmwareInput.files && firmwareInput.files[0];
  otaFileInfo.textContent = file ? file.name + " · " + formatBytes(file.size) : "Chưa chọn tệp.";
  otaProgress.style.width = "0%";
  otaResult.textContent = "";
});

async function waitForRunningBuild(expectedBuild, previousPartition) {
  otaResult.textContent = "Firmware đã được ghi. Đang chờ ESP32 khởi động và xác nhận bản dựng mới...";
  let lastBuild = null;
  for (let attempt = 0; attempt < 30; attempt++) {
    try {
      const response = await fetch("/api/firmware/status", {cache:"no-store"});
      if (response.ok) {
        const data = await response.json();
        lastBuild = Number(data.build_number || 0);
        currentBuildNumber = lastBuild;
        firmwareStatus.textContent = "Bản dựng firmware: #" + lastBuild +
          " · Phân vùng đang chạy: " + (data.running_partition || "không rõ") +
          (data.installed_firmware ? " · Tệp: " + data.installed_firmware : "");
        if (lastBuild === expectedBuild && data.running_partition &&
            data.running_partition !== previousPartition) {
          otaResult.textContent = "Cập nhật thành công. ESP32 đang chạy Firmware Build #" + expectedBuild + ".";
          return true;
        }
      }
    } catch (_) {}
    await new Promise(resolve => setTimeout(resolve, 1000));
  }
  otaResult.textContent = lastBuild === expectedBuild
    ? "ESP32 đã chạy đúng Build #" + expectedBuild + " nhưng chưa xác nhận được thay đổi phân vùng."
    : "ESP32 chưa chuyển sang Build #" + expectedBuild + ". Hiện thiết bị báo Build #" +
      (lastBuild === null ? "không xác định" : lastBuild) +
      ". Cần kiểm tra log để phân biệt lỗi ghi/chọn phân vùng với rollback khi khởi động.";
  return false;
}

updateFirmwareButton.addEventListener("click", async () => {
  const file = firmwareInput.files && firmwareInput.files[0];
  if (!file) { otaResult.textContent = "Vui lòng chọn tệp BIN hoặc ZIP."; return; }
  updateFirmwareButton.disabled = true;
  otaProgress.style.width = "0%";
  try {
    let firmware = file;
    let firmwareName = file.name;
    if (/\.zip$/i.test(file.name)) {
      otaResult.textContent = "Đang kiểm tra cấu trúc ZIP...";
      const extracted = await extractOtaBinFromZip(file);
      firmware = extracted.blob;
      firmwareName = extracted.name;
    } else if (!/\.bin$/i.test(file.name)) {
      throw new Error("Chỉ chấp nhận tệp .bin hoặc .zip.");
    }
    if (firmware.size < 4096) throw new Error("Firmware quá nhỏ hoặc không hợp lệ.");
    if (!otaMaxBytes) throw new Error("Không đọc được dung lượng phân vùng OTA. Hãy tải lại trang.");
    if (firmware.size > otaMaxBytes) throw new Error("Firmware vượt giới hạn phân vùng OTA (" + formatBytes(otaMaxBytes) + ").");
    otaResult.textContent = "Đang kiểm tra cấu trúc và số bản dựng firmware...";
    const targetBuild = await readFirmwareBuild(firmware);
    otaFileInfo.textContent = firmwareName + " · " + formatBytes(firmware.size) +
      " · Firmware Build #" + targetBuild;
    if (!confirm("Cài đặt " + firmwareName + " (" + formatBytes(firmware.size) +
      ")\nBản dựng hiện tại: #" + currentBuildNumber + "\nBản dựng mục tiêu: #" + targetBuild +
      "\nKhông tắt nguồn trong quá trình cập nhật. Tiếp tục?")) {
      updateFirmwareButton.disabled = false;
      return;
    }
    const beforeResponse = await fetch("/api/firmware/status", {cache:"no-store"});
    if (!beforeResponse.ok) throw new Error("Không đọc được phân vùng firmware đang chạy.");
    const beforeStatus = await beforeResponse.json();
    const previousPartition = beforeStatus.running_partition;
    if (!previousPartition) throw new Error("Không xác định được phân vùng firmware hiện tại.");
    otaResult.textContent = "Đang tải firmware lên thiết bị...";
    await uploadOtaFirmware(firmware, firmwareName);
    otaProgress.style.width = "100%";
    await waitForRunningBuild(targetBuild, previousPartition);
    updateFirmwareButton.disabled = false;
  } catch (error) {
    otaResult.textContent = error.message;
    updateFirmwareButton.disabled = false;
  }
});

const promptTemplates = {
personal:"Bạn là trợ lý cá nhân của tôi. Giao tiếp tự nhiên, rõ ràng bằng tiếng Việt. Trả lời ngắn gọn khi câu hỏi đơn giản; phân tích kỹ khi cần. Hỏi lại nếu thiếu dữ kiện. Không bịa thông tin.",
companion:"Bạn là người bạn đồng hành AI ấm áp, tinh tế và vui vẻ. Biết lắng nghe, quan tâm nhưng không giả vờ là con người. Trò chuyện tự nhiên bằng tiếng Việt.",
tech:"Bạn là chuyên gia công nghệ và hệ thống nhúng. Phân tích dựa trên dữ liệu, nêu rõ giả định và rủi ro. Ưu tiên giải pháp đơn giản, ổn định. Không tự bịa thông số.",
coding:"Bạn là trợ lý lập trình cấp cao. Đọc code và ngữ cảnh trước khi đề xuất sửa. Ưu tiên thay đổi nhỏ, dễ bảo trì, không thêm dependency không cần thiết.",
language:"Bạn là giáo viên ngoại ngữ kiên nhẫn. Điều chỉnh độ khó theo người học, sửa lỗi nhẹ nhàng, đưa ví dụ thực tế và giải thích bằng tiếng Việt khi cần.",
support:"Bạn là trợ lý chăm sóc khách hàng chuyên nghiệp. Lắng nghe, xác nhận nhu cầu, hướng dẫn từng bước. Không hứa điều chưa chắc chắn."
};
function updatePromptBytes(){
 const bytes=new TextEncoder().encode(promptInput.value).length;
 promptBytes.textContent=bytes.toLocaleString("vi-VN");
 promptMaxBytesLabel.textContent=maxPromptBytes?maxPromptBytes.toLocaleString("vi-VN"):"không khả dụng";
}
promptInput.addEventListener("input",updatePromptBytes);
promptTemplate.addEventListener("change",()=>{if(promptTemplate.value!=="custom"){promptInput.value=promptTemplates[promptTemplate.value]||"";updatePromptBytes();}});
async function loadGeminiSettings(){
 try{const response=await fetch("/api/gemini/settings",{cache:"no-store"});const data=await response.json();if(!response.ok)throw new Error(data.error||"Không tải được cấu hình Gemini");voiceSelect.value=data.voice||"Kore";promptInput.value=data.system_prompt||"";maxPromptBytes=Number(data.max_prompt_bytes)||0;updatePromptBytes();}
 catch(error){voiceResult.textContent=error.message;}
}
async function saveGeminiSetting(url,payload,resultElement,button){
 button.disabled=true;resultElement.textContent="Đang lưu...";
 try{const response=await fetch(url,{method:"POST",cache:"no-store",headers:{"Content-Type":"application/json","X-ChatAI-CSRF":csrfToken},body:JSON.stringify(payload)});const data=await response.json().catch(()=>({}));if(!response.ok)throw new Error(data.error||("Lưu thất bại (HTTP "+response.status+")"));resultElement.textContent=data.message||"Đã lưu. Gemini Live sẽ tạo phiên mới để áp dụng.";}
 catch(error){resultElement.textContent=error.message;}finally{button.disabled=false;}
}
document.getElementById("saveVoice").addEventListener("click",()=>saveGeminiSetting("/api/gemini/voice",{voice:voiceSelect.value},voiceResult,document.getElementById("saveVoice")));
document.getElementById("savePrompt").addEventListener("click",async()=>{
 const bytes=new TextEncoder().encode(promptInput.value).length;
 if(bytes===0){promptResult.textContent="System Prompt không được để trống.";return;}
 if(!maxPromptBytes){promptResult.textContent="Không thể xác nhận dung lượng SPIFFS assets. Hãy kiểm tra log ChatAI-Settings để biết lỗi partition, mount hoặc dung lượng.";return;}
 if(bytes>maxPromptBytes){promptResult.textContent="Prompt hiện có "+bytes+" byte, vượt dung lượng lưu tối đa "+maxPromptBytes+" byte của thiết bị.";return;}
 const button=document.getElementById("savePrompt");
 button.disabled=true;promptResult.textContent="Đang lưu System Prompt...";
 try{
  const response=await fetch("/api/gemini/prompt",{method:"POST",cache:"no-store",headers:{"Content-Type":"text/plain; charset=utf-8","X-ChatAI-CSRF":csrfToken},body:promptInput.value});
  const data=await response.json().catch(()=>({}));
  if(!response.ok)throw new Error(data.error||("Lưu System Prompt thất bại (HTTP "+response.status+")"));
  promptResult.textContent=data.message||"Đã lưu System Prompt.";
 }catch(error){promptResult.textContent=error.message;}
 finally{button.disabled=false;}
});
document.getElementById("resetPrompt").addEventListener("click",()=>{promptInput.value="Bạn là trợ lý ChatAI TPIOS. Hãy giao tiếp tự nhiên, thân thiện và rõ ràng bằng tiếng Việt. Chỉ trả lời bằng giọng nói trong phiên hội thoại thời gian thực.";promptTemplate.value="custom";updatePromptBytes();});
document.getElementById("refreshDashboard").addEventListener("click",()=>{refreshDeviceStatus().catch(error=>{deviceStatus.textContent=error.message;});refreshBluetoothOverview();});
function formatOverviewBytes(bytes) {
  const value = Number(bytes);
  if (!Number.isFinite(value) || value < 0) return "—";
  return value >= 1048576 ? (value / 1048576).toFixed(2) + " MB" : Math.round(value / 1024) + " KB";
}
async function refreshBluetoothOverview() {
  const stateLabel = document.getElementById("dashBleState");
  const countLabel = document.getElementById("dashBleCount");
  try {
    const response = await fetch("/api/bluetooth/status", {cache:"no-store"});
    const data = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(data.error || "Không đọc được BLE");
    stateLabel.textContent = data.scanning ? "Đang quét" : data.ready ? "Sẵn sàng" :
      data.initialized ? "Đã khởi tạo" : data.error_code ? "Lỗi" : "Chưa khởi tạo";
    countLabel.textContent = String(Number(data.device_count || 0));
  } catch (_) {
    stateLabel.textContent = "Không khả dụng";
    countLabel.textContent = "—";
  }
}
async function refreshDeviceStatus() {
  const response = await fetch("/api/device/status", {cache:"no-store"});
  if (!response.ok) throw new Error("Không đọc được trạng thái thiết bị (HTTP " + response.status + ")");
  const data = await response.json();
  let state;
  if (!data.wifi_connected) state = "Chưa kết nối Wi-Fi";
  else if (!data.internet_available) state = "Wi-Fi đã kết nối · Chưa xác minh Internet";
  else if (data.xiaozhi_ready) state = data.xiaozhi_listening ? "Đang hoạt động" : "Đã kết nối mạng";
  else state = "Đang chờ kết nối Xiaozhi";
  if (!speakerVolumeDirty && !speakerVolumeSaving && document.activeElement !== speakerVolumeInput) {
    speakerVolumeInput.value = String(Number(data.speaker_volume ?? 100));
    speakerVolumeValue.textContent = speakerVolumeInput.value + "%";
  }
  if (!micGainDirty && !micGainSaving && document.activeElement !== micGainInput) {
    micGainInput.value = String(Number(data.mic_gain_db ?? 0));
    micGainValue.textContent = micGainInput.value + " dB";
  }
  deviceStatus.textContent = state + (data.ip ? " · IP " + data.ip : "");
  document.getElementById("overviewDeviceState").textContent = state;
  document.getElementById("overviewBuildChip").textContent = "Build #" + Number(data.build_number || 0);
  document.getElementById("overviewUpdated").textContent = "Cập nhật lúc " +
    new Date().toLocaleTimeString("vi-VN", {hour:"2-digit", minute:"2-digit", second:"2-digit"});
  wifiAdminStatus.textContent = state + (data.ip ? " · IP: " + data.ip : "");
  document.getElementById("dashSsid").textContent = data.ssid || (data.wifi_connected ? "Đã kết nối" : "Chưa kết nối");
  document.getElementById("dashRssi").textContent = data.wifi_connected ? Number(data.rssi) + " dBm" : "—";
  document.getElementById("dashIp").textContent = data.ip || "Chưa có IP";
  document.getElementById("dashGateway").textContent = (data.gateway || "—") + " / " + (data.dns || "—");
  document.getElementById("dashInternet").textContent = data.internet_available ? "Đã xác minh" : "Chưa xác minh";
  document.getElementById("dashDnsProbe").textContent = data.dns_probe_state === 1 ? "Thành công" : data.dns_probe_state === 0 ? "Thất bại" : "Chưa xác định";
  document.getElementById("dashTcpProbe").textContent = data.tcp443_probe_state === 1 ? "Thông" : data.tcp443_probe_state === 0 ? "Không thông" : "Chưa xác định";
  document.getElementById("dashAp").textContent = data.softap_ip || "Không hoạt động";
  document.getElementById("dashXiaozhi").textContent = data.xiaozhi_ready
    ? (data.xiaozhi_listening ? "Đã kết nối · Đang nghe" : data.xiaozhi_chat_enabled ? "Đã kết nối · Chưa nghe" : "Đã kết nối")
    : "Chưa kết nối";
  document.getElementById("dashMicState").textContent = data.microphone_enabled ? "Đang bật" : "Đang tắt";
  document.getElementById("dashSpeakerState").textContent = data.speaker_enabled ? "Đang bật" : "Đang tắt";
  document.getElementById("dashVolume").textContent = Number(data.speaker_volume) + "%";
  document.getElementById("dashHeap").textContent = formatOverviewBytes(data.free_heap);
  document.getElementById("dashMinHeap").textContent = formatOverviewBytes(data.min_free_heap);
  const uptime = Number(data.uptime_seconds || 0);
  const days = Math.floor(uptime / 86400);
  const hours = Math.floor((uptime % 86400) / 3600);
  const minutes = Math.floor((uptime % 3600) / 60);
  document.getElementById("dashUptime").textContent = (days ? days + " ngày " : "") + hours + " giờ " + minutes + " phút";
  document.getElementById("dashBuild").textContent = "#" + Number(data.build_number || 0);
}

async function refreshStatus() {
  const response = await fetch("/api/gemini/status", {cache:"no-store"});
  if (!response.ok) throw new Error("Không đọc được trạng thái (HTTP " + response.status + ")");
  const data = await response.json();
  statusText.textContent = data.key_configured ? "Gemini Live đã tắt; API Key không được sử dụng." :
    "Gemini Live đã tắt; API Key không được sử dụng.";
  removeButton.hidden = !data.key_configured;
  try { await refreshDeviceStatus(); }
  catch (error) { deviceStatus.textContent = error.message; }
  try { await refreshFirmwareStatus(); }
  catch (error) { firmwareStatus.textContent = error.message; }
  await loadGeminiSettings();
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
retryGeminiButton.addEventListener("click", async () => {
  retryGeminiButton.disabled = true;
  try {
    const response = await fetch("/api/gemini/reconnect", {
      method:"POST", cache:"no-store", headers:{"X-ChatAI-CSRF":csrfToken}
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(data.error || ("Thử lại thất bại (HTTP " + response.status + ")"));
    deviceStatus.textContent = data.message || "Đã yêu cầu Gemini kết nối lại.";
    await refreshDeviceStatus();
  } catch (error) {
    deviceStatus.textContent = error.message;
  } finally {
    retryGeminiButton.disabled = false;
  }
});
restartDeviceButton.addEventListener("click", async () => {
  if (!confirm("Bạn có chắc chắn muốn khởi động lại ESP32? Kết nối Wi-Fi và Web Admin sẽ tạm thời bị gián đoạn.")) return;
  restartDeviceButton.disabled = true;
  restartResult.textContent = "Đang gửi lệnh khởi động lại...";
  try {
    const response = await fetch("/api/device/restart", {
      method:"POST", cache:"no-store", headers:{"X-ChatAI-CSRF":csrfToken}
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(data.error || ("Khởi động lại thất bại (HTTP " + response.status + ")"));
    restartResult.textContent = data.message || "ESP32 đã nhận lệnh và đang khởi động lại.";
  } catch (error) {
    restartResult.textContent = "Không nhận được phản hồi xác nhận. Hãy chờ thiết bị khởi động lại rồi tải lại Web Admin. Chi tiết: " + error.message;
  }
});
saveButton.addEventListener("click", async () => {
  const apiKey = keyInput.value.trim();
  if (!apiKey) { resultText.textContent = "Vui lòng nhập Gemini API Key."; return; }
  saveButton.disabled = true;
  resultText.textContent = "Đang lưu API Key...";
  try {
    const data = await sendKeyRequest("POST", {api_key:apiKey});
    keyInput.value = "";
    resultText.textContent = data.message || "Đã lưu API Key vào thiết bị; Gemini Live hiện đang bị tắt.";
    await refreshStatus();
  } catch (error) {
    resultText.textContent = error.message;
  } finally { saveButton.disabled = false; }
});
removeButton.addEventListener("click", async () => {
  if (!confirm("Bạn có chắc chắn muốn xóa Gemini API Key đã lưu trên thiết bị?")) return;
  removeButton.disabled = true;
  resultText.textContent = "Đang xóa API Key...";
  try {
    const data = await sendKeyRequest("DELETE");
    resultText.textContent = data.message || "Đã xóa API Key khỏi thiết bị; Gemini Live hiện đang bị tắt trong firmware. Gemini Live hiện đang bị tắt.";
    await refreshStatus();
  } catch (error) {
    resultText.textContent = error.message;
  } finally { removeButton.disabled = false; }
});
refreshStatus().catch(error => { statusText.textContent = error.message; });
refreshBluetoothOverview();
refreshDiagnostics().catch(error => { diagnosticLog.textContent = error.message; });
setInterval(() => {
  refreshDeviceStatus().catch(error => { deviceStatus.textContent = error.message; });
  refreshDiagnostics().catch(error => { diagnosticResult.textContent = error.message; });
}, 5000);
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
    const size_t host_length = httpd_req_get_hdr_value_len(request, "Host");
    if (host_length == 0 || host_length >= 96) return false;

    char host[96]{};
    if (httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }

    const size_t origin_length = httpd_req_get_hdr_value_len(request, "Origin");
    // Some mobile browser/webview requests omit Origin for same-origin fetches.
    // IsAuthorizedMutation still requires the unpredictable per-boot CSRF token.
    if (origin_length == 0) return true;
    if (origin_length >= 160) return false;

    char origin[160]{};
    if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK) {
        return false;
    }

    const std::string expected = std::string("http://") + host;
    if (origin == expected) return true;

    // Normalize the optional explicit default HTTP port on either header.
    if (expected.size() > 3 &&
        expected.compare(expected.size() - 3, 3, ":80") == 0) {
        return origin == expected.substr(0, expected.size() - 3);
    }
    return origin == expected + ":80";
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

bool ReadBody(httpd_req_t* request, std::string& body,
              size_t max_size = kMaxRequestBody) {
    if (request->content_len <= 0 ||
        static_cast<size_t>(request->content_len) > max_size) return false;
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
    const char* marker = std::strstr(kAdminPage, "__CSRF_TOKEN__");
    if (marker == nullptr) return ESP_FAIL;

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    SetNoStore(request);
    httpd_resp_set_hdr(request, "Content-Security-Policy",
        "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
        "connect-src 'self'; base-uri 'none'; frame-ancestors 'none'");

    // Stream the immutable page around the per-boot CSRF token. Avoid copying
    // the entire HTML document into a temporary std::string on the HTTP task.
    const size_t prefix_size = static_cast<size_t>(marker - kAdminPage);
    esp_err_t result = httpd_resp_send_chunk(request, kAdminPage, prefix_size);
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, g_csrf_token.c_str(),
                                       g_csrf_token.size());
    }
    if (result == ESP_OK) {
        const char* suffix = marker + std::strlen("__CSRF_TOKEN__");
        result = httpd_resp_send_chunk(request, suffix, HTTPD_RESP_USE_STRLEN);
    }
    if (result == ESP_OK) {
        result = httpd_resp_send_chunk(request, nullptr, 0);
    }
    return result;
}

esp_err_t FirmwareStatusHandler(httpd_req_t* request) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    const char* running_label = running != nullptr ? running->label : "unknown";
    const char* target_label = target != nullptr && target != running ? target->label : "unavailable";
    const size_t target_size = target != nullptr && target != running ? target->size : 0;
    const std::string installed_firmware = chatai::settings::GetInstalledFirmwareName();
    char body[384]{};
    std::snprintf(body, sizeof(body),
        "{\"build_number\":%lu,\"build_marker\":\"%s\","
        "\"running_partition\":\"%s\",\"ota_target\":\"%s\","
        "\"ota_max_bytes\":%u,\"installed_firmware\":\"%s\"}",
        static_cast<unsigned long>(chatai::firmware::kBuildNumber),
        chatai::firmware::kBuildMarker, running_label, target_label,
        static_cast<unsigned>(target_size), installed_firmware.c_str());
    return SendJson(request, "200 OK", body);
}
esp_err_t DeviceStatusHandler(httpd_req_t* request) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return SendError(request, "500 Internal Server Error", "Không đủ bộ nhớ");
    const bool wifi = chatai::network::NetworkManager::IsConnected();
    cJSON_AddBoolToObject(root, "wifi_connected", wifi);
    cJSON_AddBoolToObject(root, "internet_available", chatai::network::NetworkManager::IsInternetAvailable());
    cJSON_AddBoolToObject(root, "key_configured", chatai::settings::HasGeminiApiKey());
    cJSON_AddBoolToObject(root, "xiaozhi_ready", chatai::xiaozhi::IsReady());
    cJSON_AddBoolToObject(root, "xiaozhi_chat_enabled", chatai::xiaozhi::IsChatEnabled());
    cJSON_AddBoolToObject(root, "xiaozhi_listening", chatai::xiaozhi::IsListening());
    cJSON_AddBoolToObject(root, "gemini_enabled", false);
    cJSON_AddBoolToObject(root, "gemini_ready", false);
    cJSON_AddBoolToObject(root, "gemini_error", false);
    cJSON_AddBoolToObject(root, "gemini_tls_fatal", false);
    const std::string ssid = chatai::network::NetworkManager::GetSsid();
    const std::string ip = chatai::network::NetworkManager::GetIpAddress();
    const std::string gateway = chatai::network::NetworkManager::GetGatewayAddress();
    const std::string dns = chatai::network::NetworkManager::GetDnsAddress();
    const std::string ap_ip = chatai::network::NetworkManager::GetSoftApIpAddress();
    cJSON_AddStringToObject(root, "ssid", ssid.c_str());
    cJSON_AddStringToObject(root, "ip", ip.c_str());
    cJSON_AddStringToObject(root, "gateway", gateway.c_str());
    cJSON_AddStringToObject(root, "dns", dns.c_str());
    cJSON_AddStringToObject(root, "softap_ip", ap_ip.c_str());
    cJSON_AddNumberToObject(root, "rssi", chatai::network::NetworkManager::GetRssi());
    cJSON_AddNumberToObject(root, "dns_probe_state", chatai::network::NetworkManager::GetDnsProbeState());
    cJSON_AddNumberToObject(root, "tcp443_probe_state", chatai::network::NetworkManager::GetTcp443ProbeState());
    cJSON_AddNumberToObject(root, "speaker_volume", chatai::audio::GetOutputVolume());
    cJSON_AddNumberToObject(root, "mic_gain_db", chatai::audio::GetInputGainDb());
    cJSON_AddBoolToObject(root, "microphone_enabled", chatai::audio::IsMicrophoneEnabled());
    cJSON_AddBoolToObject(root, "speaker_enabled", chatai::audio::IsSpeakerEnabled());
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime_seconds", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "build_number", chatai::firmware::kBuildNumber);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) return SendError(request, "500 Internal Server Error", "Không thể tạo trạng thái thiết bị");
    const esp_err_t result = SendJson(request, "200 OK", json);
    cJSON_free(json);
    return result;
}

esp_err_t BluetoothStatusHandler(httpd_req_t* request) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return SendError(request, "500 Internal Server Error", "Không đủ bộ nhớ");
    const bool initialized = chatai::bluetooth::IsInitialized();
    const bool ready = chatai::bluetooth::IsReady();
    const bool scanning = chatai::bluetooth::IsScanning();
    cJSON_AddBoolToObject(root, "initialized", initialized);
    cJSON_AddBoolToObject(root, "ready", ready);
    const int error_code = chatai::bluetooth::GetLastError();
    cJSON_AddBoolToObject(root, "scanning", scanning);
    cJSON_AddBoolToObject(root, "scan_requested", chatai::bluetooth::IsScanRequested());
    cJSON_AddNumberToObject(root, "device_count", chatai::bluetooth::GetDeviceCount());
    cJSON_AddNumberToObject(root, "error_code", error_code);
    const char* message = error_code != 0 ? "BLE Scanner gặp lỗi; xem mã lỗi." :
        scanning ? "Đang quét BLE thụ động (tối đa 10 giây)." :
        initialized && !ready ? "Đang chờ NimBLE host khởi tạo." :
        ready ? "BLE Scanner sẵn sàng." :
        "Bluetooth chưa được khởi tạo.";
    cJSON_AddStringToObject(root, "message", message);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) return SendError(request, "500 Internal Server Error", "Không thể tạo trạng thái Bluetooth");
    const esp_err_t result = SendJson(request, "200 OK", json);
    cJSON_free(json);
    return result;
}

esp_err_t BluetoothDevicesHandler(httpd_req_t* request) {
    chatai::bluetooth::Device devices[chatai::bluetooth::kMaxDevices]{};
    const size_t count = chatai::bluetooth::GetDevices(devices, chatai::bluetooth::kMaxDevices);
    cJSON* root = cJSON_CreateObject();
    cJSON* items = cJSON_CreateArray();
    if (root == nullptr || items == nullptr) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        return SendError(request, "500 Internal Server Error", "Không đủ bộ nhớ");
    }
    for (size_t i = 0; i < count; ++i) {
        cJSON* item = cJSON_CreateObject();
        if (item == nullptr) continue;
        cJSON_AddStringToObject(item, "name", devices[i].name);
        cJSON_AddStringToObject(item, "address", devices[i].address);
        cJSON_AddStringToObject(item, "address_type", devices[i].address_type);
        cJSON_AddStringToObject(item, "manufacturer_data", devices[i].manufacturer_data);
        cJSON_AddStringToObject(item, "service_uuids", devices[i].service_uuids);
        cJSON_AddStringToObject(item, "advertising_data", devices[i].advertising_data);
        cJSON_AddNumberToObject(item, "rssi", devices[i].rssi);
        cJSON_AddNumberToObject(item, "last_seen_seconds", devices[i].last_seen_seconds);
        cJSON_AddItemToArray(items, item);
    }
    cJSON_AddItemToObject(root, "devices", items);
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) return SendError(request, "500 Internal Server Error", "Không thể tạo danh sách Bluetooth");
    const esp_err_t result = SendJson(request, "200 OK", json);
    cJSON_free(json);
    return result;
}

esp_err_t BluetoothScanStartHandler(httpd_req_t* request) {
    chatai::diagnostics::Add("BLE_HTTP_START", "RECEIVED", "POST /api/bluetooth/scan/start");
    if (!IsAuthorizedMutation(request)) {
        chatai::diagnostics::Add("BLE_HTTP_START", "DENIED",
                                 "Origin/Host or CSRF validation failed");
        return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    }

    const esp_err_t result = chatai::bluetooth::StartScan();
    if (result != ESP_OK) {
        char detail[80]{};
        std::snprintf(detail, sizeof(detail), "StartScan=%s",
                      esp_err_to_name(result));
        chatai::diagnostics::Add("BLE_HTTP_START", "FAIL", detail);
        ESP_LOGW(kTag, "BLE scan start failed: %s", esp_err_to_name(result));
        return SendError(request, "503 Service Unavailable", "Không thể khởi tạo BLE Scanner");
    }

    chatai::diagnostics::Add("BLE_HTTP_START", "ACCEPTED",
                             "StartScan returned ESP_OK");
    const esp_err_t response_result = SendJson(
        request, "200 OK",
        "{\"message\":\"Đã nhận yêu cầu quét BLE. Thiết bị đang khởi tạo hoặc quét trong tối đa 10 giây.\"}");
    chatai::diagnostics::Add("BLE_HTTP_START",
                             response_result == ESP_OK ? "RESPONSE_SENT" : "SEND_FAIL",
                             esp_err_to_name(response_result));
    return response_result;
}

esp_err_t BluetoothScanStopHandler(httpd_req_t* request) {
    chatai::diagnostics::Add("BLE_HTTP_STOP", "RECEIVED", "POST /api/bluetooth/scan/stop");
    if (!IsAuthorizedMutation(request)) {
        chatai::diagnostics::Add("BLE_HTTP_STOP", "DENIED",
                                 "Origin/Host or CSRF validation failed");
        return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    }

    const esp_err_t result = chatai::bluetooth::StopScan();
    if (result != ESP_OK) {
        chatai::diagnostics::Add("BLE_HTTP_STOP", "FAIL", esp_err_to_name(result));
        return SendError(request, "503 Service Unavailable", "Không thể dừng BLE Scanner");
    }

    const esp_err_t response_result = SendJson(
        request, "200 OK", "{\"message\":\"Đã yêu cầu dừng quét BLE.\"}");
    chatai::diagnostics::Add("BLE_HTTP_STOP",
                             response_result == ESP_OK ? "RESPONSE_SENT" : "SEND_FAIL",
                             esp_err_to_name(response_result));
    return response_result;
}

esp_err_t DiagnosticsHandler(httpd_req_t* request) {
    const std::string log = chatai::diagnostics::Get();
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    SetNoStore(request);
    return httpd_resp_send(request, log.c_str(), log.size());
}

esp_err_t DiagnosticsDownloadHandler(httpd_req_t* request) {
    const std::string log = chatai::diagnostics::Get();
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(request, "Content-Disposition",
                       "attachment; filename=chatai-diagnostics.txt");
    SetNoStore(request);
    return httpd_resp_send(request, log.c_str(), log.size());
}

esp_err_t DiagnosticsClearHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    }
    chatai::diagnostics::Clear();
    return SendJson(request, "200 OK", "{\"message\":\"Đã xóa log.\"}");
}

esp_err_t GeminiSettingsHandler(httpd_req_t* request) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) return SendError(request, "500 Internal Server Error", "Không đủ bộ nhớ");
    const std::string voice = chatai::settings::GetGeminiVoice();
    const std::string prompt = chatai::settings::GetGeminiSystemPrompt();
    cJSON_AddStringToObject(root, "voice", voice.c_str());
    cJSON_AddStringToObject(root, "system_prompt", prompt.c_str());
    cJSON_AddNumberToObject(root, "max_prompt_bytes", chatai::settings::GetMaxGeminiSystemPromptBytes());
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) return SendError(request, "500 Internal Server Error", "Không thể tạo cấu hình");
    const esp_err_t result = SendJson(request, "200 OK", json);
    cJSON_free(json);
    return result;
}

esp_err_t GeminiVoicePostHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    std::string body;
    if (!ReadBody(request, body)) return SendError(request, "400 Bad Request", "Dữ liệu giọng nói không hợp lệ");
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    const cJSON* item = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "voice") : nullptr;
    const std::string voice = cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
    if (root != nullptr) cJSON_Delete(root);
    if (!chatai::settings::IsSupportedGeminiVoice(voice)) return SendError(request, "400 Bad Request", "Giọng nói Gemini Live không hợp lệ");
    const esp_err_t result = chatai::settings::SaveGeminiVoice(voice);
    if (result != ESP_OK) return SendError(request, "500 Internal Server Error", "Không thể lưu giọng nói vào NVS");
    chatai::display::Notify("WEB ADMIN", "Đã lưu cấu hình");
    return SendJson(request, "200 OK", "{\"message\":\"Đã lưu cấu hình giọng nói; Gemini Live hiện đang bị tắt trong firmware.\"}");
}

esp_err_t GeminiPromptPostHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");

    const size_t max_prompt_bytes = chatai::settings::GetMaxGeminiSystemPromptBytes();
    if (max_prompt_bytes == 0) {
        return SendError(request, "503 Service Unavailable",
                         "Phân vùng assets chưa sẵn sàng; firmware không tự format để bảo vệ dữ liệu.");
    }

    char content_type[64]{};
    const size_t content_type_length =
        httpd_req_get_hdr_value_len(request, "Content-Type");
    const bool plain_text = content_type_length > 0 &&
        content_type_length < sizeof(content_type) &&
        httpd_req_get_hdr_value_str(request, "Content-Type",
                                    content_type, sizeof(content_type)) == ESP_OK &&
        std::strncmp(content_type, "text/plain", 10) == 0;

    std::string prompt;
    if (plain_text) {
        if (!ReadBody(request, prompt, max_prompt_bytes)) {
            return SendError(request, "413 Payload Too Large",
                             "Prompt vượt dung lượng lưu tối đa trên thiết bị.");
        }
    } else {
        // Keep the previous JSON request format working for older Web Admin clients.
        std::string body;
        if (!ReadBody(request, body)) {
            return SendError(request, "413 Payload Too Large", "Prompt vượt giới hạn dữ liệu");
        }
        cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
        const cJSON* item = cJSON_IsObject(root)
            ? cJSON_GetObjectItemCaseSensitive(root, "system_prompt") : nullptr;
        const bool valid = cJSON_IsString(item) && item->valuestring != nullptr;
        if (valid) prompt = item->valuestring;
        if (root != nullptr) cJSON_Delete(root);
        if (!valid) {
            return SendError(request, "400 Bad Request", "System Prompt không hợp lệ");
        }
    }

    if (prompt.empty()) {
        return SendError(request, "400 Bad Request", "System Prompt không được để trống");
    }
    if (prompt.size() > max_prompt_bytes) {
        return SendError(request, "413 Payload Too Large",
                         "System Prompt vượt dung lượng lưu tối đa trên thiết bị.");
    }

    const esp_err_t result = chatai::settings::SaveGeminiSystemPrompt(prompt);
    if (result == ESP_ERR_INVALID_SIZE) {
        return SendError(request, "413 Payload Too Large",
                         "System Prompt vượt dung lượng lưu tối đa trên thiết bị.");
    }
    if (result != ESP_OK) {
        return SendError(request, "500 Internal Server Error",
                         "Không thể ghi đầy đủ System Prompt vào phân vùng assets.");
    }
    chatai::display::Notify("WEB ADMIN", "Đã lưu cấu hình");
    return SendJson(request, "200 OK",
        R"({"message":"Đã lưu System Prompt. Gemini Live hiện đang bị tắt trong firmware."})");
}

esp_err_t GeminiStatusHandler(httpd_req_t* request) {
    const char* status = chatai::settings::HasGeminiApiKey()
        ? "{\"key_configured\":true}"
        : "{\"key_configured\":false}";
    return SendJson(request, "200 OK", status);
}

esp_err_t GeminiKeyPostHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    }

    std::string body;
    if (!ReadBody(request, body)) {
        return SendError(request, "400 Bad Request", "Dữ liệu gửi lên không hợp lệ");
    }

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (!cJSON_IsObject(root)) {
        if (root != nullptr) cJSON_Delete(root);
        return SendError(request, "400 Bad Request", "JSON không hợp lệ");
    }

    const cJSON* key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    const bool valid = cJSON_IsString(key) && IsValidApiKey(key->valuestring);
    const std::string api_key = valid ? key->valuestring : "";
    cJSON_Delete(root);
    if (!valid) {
        return SendError(request, "400 Bad Request", "Gemini API Key không hợp lệ");
    }

    const esp_err_t result = chatai::settings::SaveGeminiApiKey(api_key);
    if (result != ESP_OK) {
        return SendError(request, "500 Internal Server Error", "Không thể lưu API Key vào NVS");
    }
    chatai::display::Notify("WEB ADMIN", "Đã lưu API Key");
    return SendJson(request, "200 OK", "{\"message\":\"Đã lưu API Key vào thiết bị; Gemini Live hiện đang bị tắt trong firmware.\"}");
}

esp_err_t AudioVolumePostHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    }
    std::string body;
    if (!ReadBody(request, body)) {
        return SendError(request, "400 Bad Request", "Dữ liệu âm lượng không hợp lệ");
    }
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    const cJSON* volume = cJSON_IsObject(root)
        ? cJSON_GetObjectItemCaseSensitive(root, "volume") : nullptr;
    const bool valid = cJSON_IsNumber(volume) && volume->valuedouble >= 0 &&
        volume->valuedouble <= 100 && volume->valuedouble == volume->valueint;
    const int value = valid ? volume->valueint : -1;
    if (root != nullptr) cJSON_Delete(root);
    if (!valid) return SendError(request, "400 Bad Request", "Âm lượng phải là số nguyên từ 0 đến 100");
    const esp_err_t result = chatai::audio::SaveOutputVolume(value);
    if (result != ESP_OK) return SendError(request, "500 Internal Server Error", "Không thể lưu âm lượng vào NVS");
    chatai::display::Notify("ÂM THANH", "Đã lưu âm lượng");
    return SendJson(request, "200 OK", "{\"message\":\"Đã lưu âm lượng loa.\"}");
}

esp_err_t MicGainPostHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    }
    std::string body;
    if (!ReadBody(request, body)) {
        return SendError(request, "400 Bad Request", "Dữ liệu độ nhạy microphone không hợp lệ");
    }
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    const cJSON* gain = cJSON_IsObject(root)
        ? cJSON_GetObjectItemCaseSensitive(root, "gain_db") : nullptr;
    const bool valid = cJSON_IsNumber(gain) && gain->valuedouble >= -6 &&
        gain->valuedouble <= 6 && gain->valuedouble == gain->valueint;
    const int value = valid ? gain->valueint : -99;
    if (root != nullptr) cJSON_Delete(root);
    if (!valid) return SendError(request, "400 Bad Request", "Độ nhạy phải là số nguyên từ -6 đến +6 dB");
    const esp_err_t result = chatai::audio::SaveInputGainDb(value);
    if (result != ESP_OK) return SendError(request, "500 Internal Server Error", "Không thể lưu độ nhạy microphone vào NVS");
    char response[96]{};
    std::snprintf(response, sizeof(response), "{\"gain_db\":%d,\"message\":\"Đã lưu độ nhạy microphone.\"}", value);
    return SendJson(request, "200 OK", response);
}

esp_err_t WifiConnectAdminHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    }
    std::string body;
    if (!ReadBody(request, body)) {
        return SendError(request, "400 Bad Request", "Dữ liệu Wi-Fi không hợp lệ");
    }
    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    const cJSON* ssid_item = cJSON_IsObject(root)
        ? cJSON_GetObjectItemCaseSensitive(root, "ssid") : nullptr;
    const cJSON* password_item = cJSON_IsObject(root)
        ? cJSON_GetObjectItemCaseSensitive(root, "password") : nullptr;
    const bool valid = cJSON_IsString(ssid_item) && cJSON_IsString(password_item) &&
        ssid_item->valuestring != nullptr && password_item->valuestring != nullptr &&
        std::strlen(ssid_item->valuestring) > 0 && std::strlen(ssid_item->valuestring) <= 32 &&
        std::strlen(password_item->valuestring) <= 64;
    const std::string ssid = valid ? ssid_item->valuestring : "";
    const std::string password = valid ? password_item->valuestring : "";
    if (root != nullptr) cJSON_Delete(root);
    if (!valid) return SendError(request, "400 Bad Request", "SSID hoặc mật khẩu Wi-Fi không hợp lệ");
    const esp_err_t result = chatai::network::NetworkManager::Connect(ssid, password);
    if (result != ESP_OK) return SendError(request, "503 Service Unavailable", "Không thể bắt đầu kết nối Wi-Fi");
    chatai::display::Notify("WI-FI", "Dang ket noi");
    return SendJson(request, "200 OK", "{\"message\":\"Đã nhận cấu hình Wi-Fi. Đang kết nối router...\"}");
}

esp_err_t GeminiReconnectHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    }
    return SendError(request, "503 Service Unavailable",
                     "Gemini Live đang bị tắt trong firmware kiểm tra phần cứng.");
}

esp_err_t DeviceRestartHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    }

    std::unique_lock<std::mutex> ota_lock(g_ota_mutex, std::try_to_lock);
    if (!ota_lock.owns_lock()) {
        return SendError(request, "409 Conflict", "Không thể khởi động lại khi đang cập nhật firmware OTA");
    }

    bool expected = false;
    if (!g_restart_pending.compare_exchange_strong(expected, true)) {
        return SendError(request, "409 Conflict", "Thiết bị đang xử lý lệnh khởi động lại");
    }

    chatai::display::Notify("WEB ADMIN", "Dang khoi dong lai", 2000);
    const esp_err_t response_result = SendJson(
        request, "200 OK",
        "{\"message\":\"ESP32 đã nhận lệnh. Thiết bị đang khởi động lại; Wi-Fi và Web Admin sẽ tạm ngắt.\"}");
    if (response_result != ESP_OK) {
        g_restart_pending.store(false);
        ESP_LOGW(kTag, "Could not send restart confirmation: %s", esp_err_to_name(response_result));
        return response_result;
    }

    ESP_LOGW(kTag, "Restart requested through authenticated Web Admin");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

esp_err_t OtaUploadHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    }
    if (request->content_len < 4096) {
        return SendError(request, "400 Bad Request", "Firmware BIN rỗng hoặc quá nhỏ");
    }
    // Firmware filename is optional metadata; OTA must also accept older Web Admin clients.
    char firmware_filename[65]{};
    const size_t filename_length = httpd_req_get_hdr_value_len(request, "X-Firmware-Filename");
    if (filename_length > 0) {
        if (filename_length >= sizeof(firmware_filename) ||
            httpd_req_get_hdr_value_str(request, "X-Firmware-Filename",
                                        firmware_filename, sizeof(firmware_filename)) != ESP_OK) {
            return SendError(request, "400 Bad Request", "Tên firmware BIN không hợp lệ");
        }
        for (size_t i = 0; i < filename_length; ++i) {
            const char c = firmware_filename[i];
            const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            if (!valid) return SendError(request, "400 Bad Request", "Tên firmware chứa ký tự không hợp lệ");
        }
        if (filename_length < 5 ||
            std::strcmp(firmware_filename + filename_length - 4, ".bin") != 0) {
            return SendError(request, "400 Bad Request", "Tên firmware phải kết thúc bằng .bin");
        }
    }

    std::unique_lock<std::mutex> lock(g_ota_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return SendError(request, "409 Conflict", "Đang có một quá trình OTA khác");
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
                         "Không tìm thấy hai phân vùng OTA tương thích");
    }
    if (static_cast<size_t>(request->content_len) > target->size) {
        return SendError(request, "413 Content Too Large",
                         "Firmware BIN lớn hơn phân vùng OTA còn trống");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t result = esp_ota_begin(
        target, static_cast<size_t>(request->content_len), &handle);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_begin failed: %s", esp_err_to_name(result));
        return SendError(request, "500 Internal Server Error",
                         "Không thể bắt đầu ghi firmware");
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
                             "Quá trình tải firmware bị gián đoạn");
        }

        result = esp_ota_write(handle, buffer, static_cast<size_t>(received));
        if (result != ESP_OK) {
            esp_ota_abort(handle);
            ESP_LOGE(kTag, "esp_ota_write failed: %s", esp_err_to_name(result));
            return SendError(request, "500 Internal Server Error",
                             "Không thể ghi firmware vào phân vùng OTA");
        }
        remaining -= static_cast<size_t>(received);
        written += static_cast<size_t>(received);
    }

    result = esp_ota_end(handle);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "OTA image validation failed: %s", esp_err_to_name(result));
        return SendError(request, "400 Bad Request",
                         "Firmware BIN không hợp lệ hoặc không tương thích");
    }

    // Selecting the verified OTA image is the critical operation. NVS filename
    // tracking is optional and must never block a valid image from booting.
    result = esp_ota_set_boot_partition(target);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Could not select OTA boot partition: %s",
                 esp_err_to_name(result));
        return SendError(request, "500 Internal Server Error",
                         "Không thể chọn firmware mới để khởi động");
    }
    if (firmware_filename[0] != '\0') {
        const esp_err_t metadata_result =
            chatai::settings::SavePendingFirmwareName(firmware_filename, target->address);
        if (metadata_result != ESP_OK) {
            ESP_LOGW(kTag, "OTA selected %s but could not save optional filename metadata: %s",
                     target->label, esp_err_to_name(metadata_result));
        }
    }

    ESP_LOGI(kTag, "OTA image accepted: %u bytes -> %s",
             static_cast<unsigned>(written), target->label);
    chatai::display::Notify("WEB ADMIN", "Đang cập nhật", 5000);
    const esp_err_t response_result = SendJson(
        request, "200 OK",
        "{\"message\":\"Firmware đã được ghi. ESP32 đang khởi động lại để xác nhận cài đặt.\"}");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return response_result;
}

esp_err_t GeminiKeyDeleteHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    }
    const esp_err_t result = chatai::settings::DeleteGeminiApiKey();
    if (result != ESP_OK) {
        return SendError(request, "500 Internal Server Error", "Không thể xóa API Key khỏi NVS");
    }
    chatai::display::Notify("WEB ADMIN", "Đã xóa API Key");
    return SendJson(request, "200 OK", "{\"message\":\"Đã xóa Gemini API Key.\"}");
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

    httpd_uri_t firmware_status_uri{};
    firmware_status_uri.uri = "/api/firmware/status";
    firmware_status_uri.method = HTTP_GET;
    firmware_status_uri.handler = FirmwareStatusHandler;

    httpd_uri_t status_uri{};
    status_uri.uri = "/api/gemini/status";
    status_uri.method = HTTP_GET;
    status_uri.handler = GeminiStatusHandler;

    httpd_uri_t gemini_settings_uri{};
    gemini_settings_uri.uri = "/api/gemini/settings";
    gemini_settings_uri.method = HTTP_GET;
    gemini_settings_uri.handler = GeminiSettingsHandler;

    httpd_uri_t gemini_voice_uri{};
    gemini_voice_uri.uri = "/api/gemini/voice";
    gemini_voice_uri.method = HTTP_POST;
    gemini_voice_uri.handler = GeminiVoicePostHandler;

    httpd_uri_t gemini_prompt_uri{};
    gemini_prompt_uri.uri = "/api/gemini/prompt";
    gemini_prompt_uri.method = HTTP_POST;
    gemini_prompt_uri.handler = GeminiPromptPostHandler;

    httpd_uri_t device_status_uri{};
    device_status_uri.uri = "/api/device/status";
    device_status_uri.method = HTTP_GET;
    device_status_uri.handler = DeviceStatusHandler;

    httpd_uri_t audio_volume_uri{};
    audio_volume_uri.uri = "/api/audio/volume";
    audio_volume_uri.method = HTTP_POST;
    audio_volume_uri.handler = AudioVolumePostHandler;

    httpd_uri_t mic_gain_uri{};
    mic_gain_uri.uri = "/api/audio/mic-gain";
    mic_gain_uri.method = HTTP_POST;
    mic_gain_uri.handler = MicGainPostHandler;

    httpd_uri_t wifi_connect_uri{};
    wifi_connect_uri.uri = "/api/wifi/connect";
    wifi_connect_uri.method = HTTP_POST;
    wifi_connect_uri.handler = WifiConnectAdminHandler;

    httpd_uri_t reconnect_uri{};
    reconnect_uri.uri = "/api/gemini/reconnect";
    reconnect_uri.method = HTTP_POST;
    reconnect_uri.handler = GeminiReconnectHandler;

    httpd_uri_t restart_uri{};
    restart_uri.uri = "/api/device/restart";
    restart_uri.method = HTTP_POST;
    restart_uri.handler = DeviceRestartHandler;

    httpd_uri_t save_uri{};
    save_uri.uri = "/api/gemini/key";
    save_uri.method = HTTP_POST;
    save_uri.handler = GeminiKeyPostHandler;

    httpd_uri_t delete_uri{};
    delete_uri.uri = "/api/gemini/key";
    delete_uri.method = HTTP_DELETE;
    delete_uri.handler = GeminiKeyDeleteHandler;

    httpd_uri_t diagnostics_uri{};
    diagnostics_uri.uri = "/api/diagnostics";
    diagnostics_uri.method = HTTP_GET;
    diagnostics_uri.handler = DiagnosticsHandler;

    httpd_uri_t diagnostics_download_uri{};
    diagnostics_download_uri.uri = "/api/diagnostics/download";
    diagnostics_download_uri.method = HTTP_GET;
    diagnostics_download_uri.handler = DiagnosticsDownloadHandler;

    httpd_uri_t diagnostics_clear_uri{};
    diagnostics_clear_uri.uri = "/api/diagnostics/clear";
    diagnostics_clear_uri.method = HTTP_POST;
    diagnostics_clear_uri.handler = DiagnosticsClearHandler;

    httpd_uri_t bluetooth_status_uri{};
    bluetooth_status_uri.uri = "/api/bluetooth/status";
    bluetooth_status_uri.method = HTTP_GET;
    bluetooth_status_uri.handler = BluetoothStatusHandler;

    httpd_uri_t bluetooth_devices_uri{};
    bluetooth_devices_uri.uri = "/api/bluetooth/devices";
    bluetooth_devices_uri.method = HTTP_GET;
    bluetooth_devices_uri.handler = BluetoothDevicesHandler;

    httpd_uri_t bluetooth_scan_start_uri{};
    bluetooth_scan_start_uri.uri = "/api/bluetooth/scan/start";
    bluetooth_scan_start_uri.method = HTTP_POST;
    bluetooth_scan_start_uri.handler = BluetoothScanStartHandler;

    httpd_uri_t bluetooth_scan_stop_uri{};
    bluetooth_scan_stop_uri.uri = "/api/bluetooth/scan/stop";
    bluetooth_scan_stop_uri.method = HTTP_POST;
    bluetooth_scan_stop_uri.handler = BluetoothScanStopHandler;

    httpd_uri_t ota_uri{};
    ota_uri.uri = "/api/ota";
    ota_uri.method = HTTP_POST;
    ota_uri.handler = OtaUploadHandler;

    esp_err_t result = httpd_register_uri_handler(server, &admin_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &firmware_status_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &status_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &gemini_settings_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &gemini_voice_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &gemini_prompt_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &audio_volume_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &mic_gain_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &wifi_connect_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &device_status_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &reconnect_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &restart_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &save_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &delete_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &diagnostics_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &diagnostics_download_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &diagnostics_clear_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &bluetooth_status_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &bluetooth_devices_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &bluetooth_scan_start_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &bluetooth_scan_stop_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &ota_uri);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Could not register Web Admin routes: %s", esp_err_to_name(result));
    }
    return result;
}

}  // namespace chatai::web_admin
