#include "web_admin.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
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
#include "gemini_live_client.h"
#include "display_manager.h"
#include "network_manager.h"
#include "firmware_info.h"
#include "diagnostic_manager.h"

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
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark">
<title>ChatAI TPIOS · Web Admin</title>
<style>
:root{color-scheme:dark;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;background:#090914;color:#f4f1ff;font-synthesis:none}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;padding:28px 16px 44px;background:radial-gradient(ellipse at 50% -10%,#38206a 0,transparent 48%),linear-gradient(160deg,#0c0b19,#111020 58%,#090914)}
main{width:100%;max-width:480px;margin:0 auto}
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
hr{border:0;border-top:1px solid #ffffff12;margin:18px 0}
#result,#otaResult{font-size:12px;line-height:1.5;color:#d8caff;min-height:1.4em;margin:12px 0 0;overflow-wrap:anywhere}
#firmwareStatus{font-size:12px;line-height:1.55;color:#d9d2f0;margin:0 0 12px;overflow-wrap:anywhere}
#otaFileInfo{margin:9px 0}
.progress{height:8px;background:#ffffff12;border-radius:20px;overflow:hidden;margin-top:13px}
#otaProgress{height:100%;width:0;background:linear-gradient(90deg,#9b5cff,#55d6c2);transition:width .12s}
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
<section class="card" aria-labelledby="connectionTitle">
<div class="card-head"><div class="card-icon" aria-hidden="true">◉</div><div><h2 id="connectionTitle">Trạng thái kết nối</h2><p class="hint">Theo dõi Wi-Fi, Internet và Gemini Live</p></div></div>
<p id="status">Đang kiểm tra trạng thái...</p>
<p id="deviceStatus" role="status" aria-live="polite">Đang kiểm tra Wi-Fi, Internet và Gemini Live...</p>
<div class="actions"><button id="retryGemini" class="secondary">Thử kết nối lại Gemini</button></div>
</section>
<section class="card" aria-labelledby="diagnosticsTitle">
<div class="card-head"><div class="card-icon" aria-hidden="true">≋</div><div><h2 id="diagnosticsTitle">Log chẩn đoán Gemini</h2><p class="hint">Tối đa 8 KB · Không lưu API Key hoặc dữ liệu âm thanh</p></div></div>
<pre id="diagnosticLog" style="white-space:pre-wrap;overflow-wrap:anywhere;max-height:260px;overflow:auto;background:#0d0c19;border:1px solid #ffffff18;border-radius:12px;padding:12px;color:#d9d2f0;font:11px/1.55 ui-monospace,monospace">Đang tải log...</pre>
<textarea id="diagnosticCopyFallback" readonly hidden aria-label="Log chẩn đoán để sao chép" style="width:100%;min-height:150px;margin-top:10px;padding:12px;border:1px solid #a477ff;border-radius:12px;background:#0d0c19;color:#f5f1ff;font:11px/1.55 ui-monospace,monospace"></textarea>
<div class="actions"><button id="copyDiagnostic" class="secondary">Sao chép log</button><button id="clearDiagnostic" class="secondary">Xóa log</button></div>
<p id="diagnosticResult" class="hint" role="status" aria-live="polite"></p>
</section>
<section class="card" aria-labelledby="geminiTitle">
<div class="card-head"><div class="card-icon" aria-hidden="true">✧</div><div><h2 id="geminiTitle">Cấu hình Gemini Live</h2><p class="hint">API Key được lưu an toàn trong bộ nhớ thiết bị</p></div></div>
<label for="apiKey">Gemini API Key</label>
<input id="apiKey" type="password" maxlength="256" autocomplete="new-password" autocapitalize="off" spellcheck="false" placeholder="Nhập API Key mới">
<p class="hint">Khóa đã lưu không được gửi ngược về trình duyệt. Để giữ khóa hiện tại, hãy để trống và không nhấn lưu.</p>
<div class="actions"><button id="save">Lưu API Key</button><button id="remove" class="secondary" hidden>Xóa API Key</button></div>
<p id="result" role="status" aria-live="polite"></p>
</section>
<section class="card" aria-labelledby="otaTitle">
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
const clearDiagnosticButton = document.getElementById("clearDiagnostic");
let otaMaxBytes = 0;
let currentBuildNumber = 0;

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
  throw new Error("Không tìm thấy số bản dựng trong firmware. Hãy chọn tệp BIN chính thức của ChatAI TPIOS.");
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

async function waitForRunningBuild(expectedBuild) {
  otaResult.textContent = "Firmware đã được ghi. Đang chờ ESP32 khởi động lại và xác nhận phiên bản...";
  await new Promise(resolve => setTimeout(resolve, 6000));
  for (let attempt = 0; attempt < 24; attempt++) {
    try {
      const response = await fetch("/api/firmware/status", {cache:"no-store"});
      if (response.ok) {
        const data = await response.json();
        const actualBuild = Number(data.build_number || 0);
        firmwareStatus.textContent = "Bản dựng firmware đang chạy: #" + actualBuild;
        if (actualBuild === expectedBuild) {
          otaResult.textContent = "Cập nhật thành công. Thiết bị đang chạy Bản dựng firmware #" + actualBuild + ".";
          return;
        }
        if (attempt > 5 && actualBuild !== expectedBuild) {
          otaResult.textContent = "Thiết bị hiện vẫn chạy Bản dựng #" + actualBuild +
            ". Firmware mới chưa được xác nhận; có thể thiết bị đã rollback.";
          return;
        }
      }
    } catch (_) {}
    await new Promise(resolve => setTimeout(resolve, 1000));
  }
  otaResult.textContent = "Chưa đọc lại được phiên bản sau khi khởi động. Hãy tải lại trang để kiểm tra số bản dựng thực tế.";
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
    otaResult.textContent = "Đang đọc số bản dựng trong firmware...";
    const targetBuild = await readFirmwareBuild(firmware);
    otaFileInfo.textContent = firmwareName + " · " + formatBytes(firmware.size) +
      " · Bản dựng firmware dự kiến: #" + targetBuild;
    const confirmText = "Cài đặt " + firmwareName + " (" + formatBytes(firmware.size) +
      ")\nBản dựng hiện tại: #" + currentBuildNumber + "\nBản dựng mục tiêu: #" + targetBuild +
      "\nKhông tắt nguồn trong quá trình cập nhật. Tiếp tục?";
    if (!confirm(confirmText)) { updateFirmwareButton.disabled = false; return; }

    otaResult.textContent = "Đang tải firmware lên thiết bị...";
    const data = await uploadOtaFirmware(firmware, firmwareName);
    otaProgress.style.width = "100%";
    otaResult.textContent = data.message || "ESP32 đã chấp nhận firmware.";
    await waitForRunningBuild(targetBuild);
    updateFirmwareButton.disabled = false;
  } catch (error) {
    otaResult.textContent = error.message;
    updateFirmwareButton.disabled = false;
  }
});

async function refreshDeviceStatus() {
  const response = await fetch("/api/device/status", {cache:"no-store"});
  if (!response.ok) throw new Error("Không đọc được trạng thái thiết bị (HTTP " + response.status + ")");
  const data = await response.json();
  let state;
  if (!data.wifi_connected) state = "Wi-Fi: Chưa kết nối";
  else if (!data.internet_available) state = "Wi-Fi: Đã kết nối · Internet: Chưa xác nhận";
  else if (!data.key_configured) state = "Internet: Đã thông · Gemini: Chưa cấu hình API Key";
  else if (data.gemini_ready) state = "Gemini Live: Đã sẵn sàng (đã nhận setupComplete)";
  else if (data.gemini_tls_fatal) state = "Gemini Live: Lỗi xác thực chứng chỉ TLS; đã tạm dừng tự kết nối lại. Hãy kiểm tra mạng/giờ hệ thống rồi thử lại.";
  else if (data.gemini_error) state = "Gemini Live: Lỗi kết nối hoặc xác thực; hãy dùng nút Thử kết nối lại hoặc cập nhật API Key";
  else state = "Gemini Live: Đang kết nối, chờ setupComplete";
  deviceStatus.textContent = state +
    (data.ip ? " · IP: " + data.ip : "") +
    (data.gateway ? " · Gateway: " + data.gateway : "");
}

async function refreshStatus() {
  const response = await fetch("/api/gemini/status", {cache:"no-store"});
  if (!response.ok) throw new Error("Không đọc được trạng thái (HTTP " + response.status + ")");
  const data = await response.json();
  statusText.textContent = data.key_configured ? "Trạng thái: API Key đã được lưu." :
    "Trạng thái: Chưa cấu hình API Key.";
  removeButton.hidden = !data.key_configured;
  try { await refreshDeviceStatus(); }
  catch (error) { deviceStatus.textContent = error.message; }
  try { await refreshFirmwareStatus(); }
  catch (error) { firmwareStatus.textContent = error.message; }
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
saveButton.addEventListener("click", async () => {
  const apiKey = keyInput.value.trim();
  if (!apiKey) { resultText.textContent = "Vui lòng nhập Gemini API Key."; return; }
  saveButton.disabled = true;
  resultText.textContent = "Đang lưu API Key...";
  try {
    const data = await sendKeyRequest("POST", {api_key:apiKey});
    keyInput.value = "";
    resultText.textContent = data.message || "Đã lưu Gemini API Key.";
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
    resultText.textContent = data.message || "Đã xóa Gemini API Key.";
    await refreshStatus();
  } catch (error) {
    resultText.textContent = error.message;
  } finally { removeButton.disabled = false; }
});
refreshStatus().catch(error => { statusText.textContent = error.message; });
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

esp_err_t FirmwareStatusHandler(httpd_req_t* request) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    const char* running_label = running != nullptr ? running->label : "unknown";
    const char* target_label = target != nullptr && target != running ? target->label : "unavailable";
    const size_t target_size = target != nullptr && target != running ? target->size : 0;
    char body[256]{};
    std::snprintf(body, sizeof(body),
        "{\"build_number\":%lu,\"build_marker\":\"%s\","
        "\"running_partition\":\"%s\",\"ota_target\":\"%s\","
        "\"ota_max_bytes\":%u}",
        static_cast<unsigned long>(chatai::firmware::kBuildNumber),
        chatai::firmware::kBuildMarker, running_label, target_label,
        static_cast<unsigned>(target_size));
    return SendJson(request, "200 OK", body);
}
esp_err_t DeviceStatusHandler(httpd_req_t* request) {
    const bool wifi_connected = chatai::network::NetworkManager::IsConnected();
    const bool internet_available = chatai::network::NetworkManager::IsInternetAvailable();
    const bool key_configured = chatai::settings::HasGeminiApiKey();
    const bool gemini_ready = chatai::gemini::IsReady();
    const bool gemini_error = chatai::gemini::HasError();
    const bool gemini_tls_fatal = chatai::gemini::IsTlsFatal();
    const std::string ip = chatai::network::NetworkManager::GetIpAddress();
    const std::string gateway = chatai::network::NetworkManager::GetGatewayAddress();

    char body[448]{};
    std::snprintf(body, sizeof(body),
        "{\"wifi_connected\":%s,\"internet_available\":%s,"
        "\"key_configured\":%s,\"gemini_ready\":%s,"
        "\"gemini_error\":%s,\"gemini_tls_fatal\":%s,"
        "\"ip\":\"%s\",\"gateway\":\"%s\"}",
        wifi_connected ? "true" : "false",
        internet_available ? "true" : "false",
        key_configured ? "true" : "false",
        gemini_ready ? "true" : "false",
        gemini_error ? "true" : "false",
        gemini_tls_fatal ? "true" : "false",
        ip.c_str(), gateway.c_str());
    return SendJson(request, "200 OK", body);
}

esp_err_t DiagnosticsHandler(httpd_req_t* request) {
    const std::string log = chatai::diagnostics::Get();
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
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
    chatai::gemini::RequestReconnect();
    chatai::display::Notify("WEB ADMIN", "Đã lưu API Key");
    return SendJson(request, "200 OK", "{\"message\":\"Đã lưu Gemini API Key.\"}");
}

esp_err_t GeminiReconnectHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    }
    if (!chatai::settings::HasGeminiApiKey()) {
        return SendError(request, "400 Bad Request", "Chưa cấu hình Gemini API Key");
    }
    chatai::gemini::RequestReconnect();
    chatai::display::Notify("GEMINI", "Dang thu ket noi lai");
    return SendJson(request, "200 OK", "{\"message\":\"Đã yêu cầu Gemini kết nối lại.\"}");
}

esp_err_t OtaUploadHandler(httpd_req_t* request) {
    if (!IsAuthorizedMutation(request)) {
        return SendError(request, "403 Forbidden", "Yêu cầu không hợp lệ");
    }
    if (request->content_len < 4096) {
        return SendError(request, "400 Bad Request", "Firmware BIN rỗng hoặc quá nhỏ");
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

    result = esp_ota_set_boot_partition(target);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Could not select OTA boot partition: %s",
                 esp_err_to_name(result));
        return SendError(request, "500 Internal Server Error",
                         "Không thể chọn firmware mới để khởi động");
    }

    result = chatai::network::NetworkManager::ClearSavedCredentials();
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Could not clear old Wi-Fi credentials for the new firmware: %s",
                 esp_err_to_name(result));
        esp_ota_set_boot_partition(running);
        return SendError(request, "500 Internal Server Error",
                         "Không thể xóa cấu hình Wi-Fi cũ; firmware mới chưa được khởi động");
    }

    ESP_LOGI(kTag, "OTA image accepted: %u bytes -> %s",
             static_cast<unsigned>(written), target->label);
    chatai::display::Notify("WEB ADMIN", "Đang cập nhật", 5000);
    const esp_err_t response_result = SendJson(
        request, "200 OK",
        "{\"message\":\"Firmware đã được xác thực và cài đặt. ESP32 đang khởi động lại.\"}");
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
    chatai::gemini::RequestReconnect();
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

    httpd_uri_t device_status_uri{};
    device_status_uri.uri = "/api/device/status";
    device_status_uri.method = HTTP_GET;
    device_status_uri.handler = DeviceStatusHandler;

    httpd_uri_t reconnect_uri{};
    reconnect_uri.uri = "/api/gemini/reconnect";
    reconnect_uri.method = HTTP_POST;
    reconnect_uri.handler = GeminiReconnectHandler;

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

    httpd_uri_t diagnostics_clear_uri{};
    diagnostics_clear_uri.uri = "/api/diagnostics/clear";
    diagnostics_clear_uri.method = HTTP_POST;
    diagnostics_clear_uri.handler = DiagnosticsClearHandler;

    httpd_uri_t ota_uri{};
    ota_uri.uri = "/api/ota";
    ota_uri.method = HTTP_POST;
    ota_uri.handler = OtaUploadHandler;

    esp_err_t result = httpd_register_uri_handler(server, &admin_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &firmware_status_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &status_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &device_status_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &reconnect_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &save_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &delete_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &diagnostics_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &diagnostics_clear_uri);
    if (result == ESP_OK) result = httpd_register_uri_handler(server, &ota_uri);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Could not register Web Admin routes: %s", esp_err_to_name(result));
    }
    return result;
}

}  // namespace chatai::web_admin
