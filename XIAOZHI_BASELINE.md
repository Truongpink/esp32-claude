# XIAOZHI_BASELINE.md — ChatAI TPIOS ESP32-S3

> Snapshot ngày 2026-11-23. Tài liệu này ghi nhận trạng thái firmware từ source trên GitHub `main`; không phải kết quả build hoặc kiểm thử phần cứng.
>
> Repository: [Truongpink/esp32-claude](https://github.com/Truongpink/esp32-claude)  
> Branch: `main`  
> HEAD khi lập tài liệu: `06baa9aca5f825d2c6557c1bdda037407f9aef47`  
> Firmware project đang duy trì: `firmware/standalone`  
> Target: ESP32-S3, ESP-IDF 6.1.

## Mục lục

1. [Quy ước mức độ tin cậy](#1-quy-ước-mức-độ-tin-cậy)
2. [Kết luận quan trọng về backend hiện tại](#2-kết-luận-quan-trọng-về-backend-hiện-tại)
3. [Sơ đồ luồng khởi động và hội thoại](#3-sơ-đồ-luồng-khởi-động-và-hội-thoại)
4. [Khởi động nền tảng và Wi-Fi](#4-khởi-động-nền-tảng-và-wi-fi)
5. [Nút BOOT và PTT](#5-nút-boot-và-ptt)
6. [Chatbot Xiaozhi và giao thức mạng](#6-chatbot-xiaozhi-và-giao-thức-mạng)
7. [Microphone, xử lý âm thanh và loa](#7-microphone-xử-lý-âm-thanh-và-loa)
8. [Thông báo bằng âm thanh](#8-thông-báo-bằng-âm-thanh)
9. [OLED, đồng hồ và IP](#9-oled-đồng-hồ-và-ip)
10. [Diagnostics và dấu vết reset](#10-diagnostics-và-dấu-vết-reset)
11. [Web Admin, NVS và OTA](#11-web-admin-nvs-và-ota)
12. [Phần cứng và giới hạn xác minh](#12-phần-cứng-và-giới-hạn-xác-minh)
13. [Lịch sử lỗi và thay đổi liên quan](#13-lịch-sử-lỗi-và-thay-đổi-liên-quan)
14. [Baseline để thay backend bằng Gemini Live](#14-baseline-để-thay-backend-bằng-gemini-live)
15. [Tài liệu nguồn và các điểm chưa thống nhất](#15-tài-liệu-nguồn-và-các-điểm-chưa-thống-nhất)

## 1. Quy ước mức độ tin cậy

- **[SOURCE-VERIFIED]**: xác minh trực tiếp trong source code ở HEAD nêu trên.
- **[DEVICE-REPORTED]**: người dùng đã báo đã thử trên thiết bị; chưa có log/video kiểm chứng kèm theo trong lần lập tài liệu này.
- **[LOG-CAPABLE]**: source có tạo log/diagnostic cho sự kiện; không đồng nghĩa đã thu được log của một lần chạy cụ thể.
- **[HISTORICAL]**: thông tin lịch sử từ commit hoặc trao đổi trước.
- **[NOT-VERIFIED]**: chưa có bằng chứng đủ để kết luận.
- **[DOC-CONFLICT]**: tài liệu trong repository mâu thuẫn với source hiện tại hoặc với tài liệu khác.

Không xem việc code có hàm khởi tạo là bằng chứng phần cứng đã hoạt động. Không xem việc tăng stack hoặc sửa code là bằng chứng lỗi đã hết trên thiết bị.

## 2. Kết luận quan trọng về backend hiện tại

**[SOURCE-VERIFIED] Ở HEAD `06baa9a`, đường khởi động trong `firmware/standalone/main/app_main.cpp` khởi tạo Xiaozhi làm voice client duy nhất.** Source có comment:

- `Xiaozhi is the sole voice client. Gemini Live remains fully disabled.`
- Khi khởi tạo thành công, diagnostic ghi `VOICE_CLIENT / XIAOZHI_ONLY` và `Gemini Live disabled; Xiaozhi session manager started`.

Client hiện tại nằm trong namespace `chatai::xiaozhi`, class nội bộ `XiaozhiClient`, dù tên file có tiền tố `chatai_xiaozhi_`. Đây không phải bằng chứng rằng firmware đang kết nối Gemini.

**[DOC-CONFLICT]** Một số tài liệu cũ trong repository (README, `docs/CHECKPOINT.md`, `docs/STANDALONE_MIGRATION.md`, `docs/GEMINI_LIVE_WIRE_PROTOCOL.md`) mô tả Gemini Live trực tiếp và/hoặc nói Xiaozhi đã bị loại bỏ. Mô tả đó không khớp với đường gọi hiện tại trong `app_main.cpp`, `button_manager.cpp`, `network_manager.cpp` và `chatai_xiaozhi_client.cpp`. Khi xác định hành vi firmware ở snapshot này, ưu tiên source code tại HEAD; không lấy nội dung tài liệu cũ làm bằng chứng runtime.

**[DEVICE-REPORTED]** Người dùng hiện tạm đánh giá chatbot đang chạy được và sẽ tiếp tục test các lỗi còn lại sau. Tài liệu này giữ nguyên ghi nhận đó, nhưng không tự nâng thành xác minh kỹ thuật độc lập.

## 3. Sơ đồ luồng khởi động và hội thoại

### Khởi động

```text
app_main()
  ├─ ReportBootDiagnostics()
  ├─ InitializePlatform()
  │    ├─ NVS
  │    ├─ esp_netif + event loop + mDNS
  │    ├─ NetworkManager::Initialize()
  │    ├─ display::Initialize()
  │    └─ audio::Initialize()
  ├─ xử lý trạng thái OTA pending-verify / rollback
  ├─ status_led::Initialize()
  ├─ buttons::Initialize()
  └─ xiaozhi::Initialize()
       ├─ XiaozhiAudioRuntime / AudioService
       └─ xiaozhi_net worker
            ├─ đợi Wi-Fi
            ├─ lấy server config qua Xiaozhi OTA discovery
            ├─ chọn MQTT + UDP hoặc WebSocket
            ├─ gửi hello và đợi server hello
            └─ session ready → xử lý audio/control
```

### Hội thoại

```text
BOOT release ──> SetChatEnabled(true) ──> nghe liên tục
PTT press/release ──> SetPushToTalk(true/false)
Microphone I2S RX
  └─ AudioService / AFE / voice processing
       └─ send queue (audio frames / Opus)
            └─ MQTT+UDP hoặc WebSocket ──> Xiaozhi server

Xiaozhi server
  ├─ control messages / trạng thái hội thoại
  └─ audio downlink (Opus hoặc negotiated payload)
       └─ AudioService decode ──> I2S speaker TX
```

Sơ đồ trên phản ánh cấu trúc source. Trạng thái kết nối và luồng âm thanh thực tế phải được xác minh bằng log hoặc thử nghiệm thiết bị.

## 4. Khởi động nền tảng và Wi-Fi

### Entry point

**[SOURCE-VERIFIED]** `firmware/standalone/main/app_main.cpp`:

- `app_main()` gọi `ReportBootDiagnostics()`, sau đó `InitializePlatform()`.
- `InitializePlatform()` khởi tạo NVS, network interface, event loop mặc định, mDNS, `NetworkManager`, OLED và I2S audio.
- NVS không tự động bị erase khi khởi tạo thất bại; source chủ động giữ lại cài đặt người dùng.
- Sau phần platform, firmware khởi tạo status LED, button manager rồi Xiaozhi client.
- Nếu OTA image ở trạng thái pending verify, source gọi `esp_ota_mark_app_valid_cancel_rollback()` sau phần platform; lỗi xác nhận dẫn tới restart.

### Wi-Fi

**[SOURCE-VERIFIED]** `firmware/standalone/main/network_manager.cpp`:

- Namespace NVS Wi-Fi: `chatai_net`; khóa SSID/password là `ssid` và `password`.
- Có trạng thái atomic cho connected, internet available, provisioning và reconnect.
- Có task reconnect với backoff; sau nhiều lần thử có thể khởi động setup AP.
- `ProbeXiaozhiServer()` kiểm tra IP/gateway, DNS `api.tenclass.net` và TCP port 443. Diagnostic hiện ghi rõ đây là kiểm tra endpoint Xiaozhi.
- Khi credentials mới được thử, source chỉ lưu chúng sau khi kiểm tra Internet thành công.
- Khi load credentials, source xóa namespace legacy `wifi` rồi đọc `chatai_net`.

**[LOG-CAPABLE]** Có diagnostic cho NETWORK, DNS, TCP_443, INTERNET, WIFI_RECONNECT và các trạng thái provisioning.

**[NOT-VERIFIED]** Chưa kiểm tra trực tiếp SSID, IP, gateway, DNS, RSSI hoặc khả năng tự reconnect trên thiết bị trong lần lập baseline này.

## 5. Nút BOOT và PTT

File nguồn: `firmware/standalone/main/button_manager.cpp`.

### Mapping theo source

| Nút | GPIO | Active level | Mục đích trong source |
|---|---:|---|---|
| BOOT | GPIO 0 | LOW | Nhấn rồi nhả để bật hội thoại liên tục |
| PTT / touch | GPIO 43 | HIGH | Push-to-talk theo trạng thái nhấn/nhả |
| Volume Up | GPIO 40 | LOW | Tăng âm lượng |
| Volume Down | GPIO 39 | LOW | Giảm âm lượng |

**[SOURCE-VERIFIED]** Đây là cấu hình firmware, không phải sơ đồ điện đã xác minh trên PCB thực tế.

### Cách đọc nút

- `ButtonTask` poll mỗi 10 ms.
- Debounce: 35 ms.
- `ConfigureButton()` cấu hình input và pull-up/pull-down theo active level.
- `UpdateButton()` chỉ phát sự kiện khi trạng thái ổn định thay đổi.
- Task được tạo bằng `xTaskCreate(ButtonTask, "chatai_buttons", 6144, ..., priority 4, ...)`.

### Hành vi BOOT

**[SOURCE-VERIFIED]**

- Khi có transition, source ghi diagnostic BOOT.
- Hành động chỉ chạy ở cạnh nhả nút (`!g_boot.stable_pressed`), không chạy ở cạnh nhấn.
- Nếu đang Wi-Fi provisioning: hiện thông báo cấu hình Wi-Fi, không bật hội thoại.
- Nếu không provisioning: nếu chat chưa bật thì gọi `SetChatEnabled(true)`; sau đó ghi nhận trạng thái enabled, server ready và listening.
- Source mô tả hành vi này là tương thích với Xiaozhi OnClick: thực hiện một lần sau khi nhả nút.

### Hành vi PTT

**[SOURCE-VERIFIED]**

- Khi GPIO43 thay đổi trạng thái, gọi `SetPushToTalk(g_touch.stable_pressed)`.
- Nhấn giữ đặt PTT true; nhả đặt PTT false.
- Nếu notify player đang phát, lệnh nhấn PTT bị bỏ qua.
- Thông báo OLED thay đổi theo ready/listening/continuous chat.

### Stack và lỗi reset lịch sử

**[HISTORICAL]** Commit `02f4ed74b85f606990dc3dbc13eac2e305407e8f` (message: `fix(buttons): increase BOOT handler task stack`) thay đổi stack ButtonTask từ 3072 lên 6144 bytes. Source ở HEAD vẫn giữ 6144.

**[DEVICE-REPORTED]** Trước đây người dùng đã báo nhấn BOOT có lúc làm ESP32-S3 khởi động lại. Việc tăng stack là thay đổi mã nguồn đã commit, không tự chứng minh hiện tượng reset đã được khắc phục trên thiết bị.

### Diagnostic nút

**[LOG-CAPABLE]** Source có:
- `BUTTON` với tên nút, GPIO, trạng thái, active level và stack watermark.
- `BOOT_PRESS`, `BOOT_RELEASE`.
- `BOOT_ACTION_BEGIN`, `BOOT_ACTION_DONE`.
- Crash context được giữ lại qua reset bằng RTC no-init memory trong `diagnostic_manager.cpp`.

## 6. Chatbot Xiaozhi và giao thức mạng

File chính: `chatai_xiaozhi_client.cpp/.h`, `chatai_xiaozhi_ota.cpp`, `chatai_xiaozhi_mqtt.cpp`, `chatai_xiaozhi_websocket.cpp`.

### Khởi tạo và readiness

**[SOURCE-VERIFIED]**

- `chatai::xiaozhi::Initialize()` khởi tạo singleton `XiaozhiClient`.
- Client đăng ký callback playback, VAD và playback-started với audio runtime.
- Audio runtime được khởi tạo trước khi tạo task `xiaozhi_net` stack 6144, priority 5.
- Worker chờ Wi-Fi connected, sau đó gọi `FetchServerConfig()` để lấy cấu hình server/transport.
- Có nhánh yêu cầu activation/binding; khi cần sẽ hiển thị activation notice/code và thử kiểm tra activation.
- Sau khi transport kết nối, client gửi hello, chờ server hello tối đa 12 giây, rồi cấu hình session.
- `IsReady()` phản ánh `session_ready_`; Wi-Fi connected riêng lẻ không đồng nghĩa server ready.

### Transport

**[SOURCE-VERIFIED]**

- Transport được chọn theo server config: MQTT + UDP hoặc WebSocket.
- MQTT dùng cấu hình broker, client ID, username/password và topic do server config cung cấp; TLS dùng certificate bundle khi endpoint yêu cầu TLS.
- WebSocket hỗ trợ ws/wss; WSS gắn certificate bundle. Header gồm Authorization token, Protocol-Version, Device-Id và Client-Id.
- Nhánh MQTT+UDP có kênh điều khiển MQTT và kênh audio UDP được mã hóa theo session.
- Nhánh WebSocket dùng message control và binary audio payload.
- Audio transport và session config do server negotiation cung cấp; không hard-code một endpoint Gemini Live trong đường khởi động hiện tại.

### Trạng thái runtime

**[SOURCE-VERIFIED]** `GetRuntimeState()` ánh xạ:
- `kConnecting`: Wi-Fi chưa kết nối hoặc session chưa ready.
- `kSpeaking`: speaker đang phát.
- `kProcessing`: TTS/turn pending.
- `kListening`: VAD đang nói hoặc capture đang bật.
- `kError`: chat/ptt được yêu cầu nhưng không ở các trạng thái trên.
- `kWaiting`: trạng thái chờ.

**[DEVICE-REPORTED]** Người dùng tạm đánh giá chatbot đang hoạt động được. Chưa có bản log phiên chat mới được đính kèm vào baseline này.

## 7. Microphone, xử lý âm thanh và loa

Files: `audio_manager.cpp`, `audio_codec.cpp`, `chatai_audio_codec_bridge.cpp`, `chatai_xiaozhi_audio_codec.cpp`, `audio_service.cc`, `audio_debugger.cc`, `afe_audio_engine.cc`, `chatai_xiaozhi_audio_runtime.cpp`.

### I2S theo source

| Thành phần | Cấu hình source |
|---|---|
| Microphone | I2S controller 1, RX, mono, 16 kHz, slot 32-bit |
| Mic BCLK / WS / DIN | GPIO 5 / GPIO 4 / GPIO 6 |
| Speaker | I2S controller 0, TX, mono, 24 kHz, slot 32-bit |
| Speaker BCLK / WS / DOUT | GPIO 15 / GPIO 16 / GPIO 7 |
| Amplifier enable | GPIO 17, cấu hình output |
| Mic DMA | 6 descriptors, 240 frames |
| Speaker DMA | 6 descriptors, 360 frames |

**[NOT-VERIFIED]** Hardware profile đánh dấu microphone GPIO 5/4/6 là provisional, chưa xác minh trên PCB thực tế. GPIO speaker và amplifier enable cũng là firmware mapping, chưa có schematic PCB chứng minh.

### Capture và PCM

**[SOURCE-VERIFIED]**

- `audio_manager.cpp` đọc sample I2S 32-bit, chuyển sang PCM16 bằng phép dịch phải 12 bit, áp dụng input gain từ -6 đến +6 dB rồi clamp về INT16.
- Mic gain mặc định 0 dB; speaker volume mặc định 100%, cả hai có thể được đọc từ NVS namespace `chatai_cfg`.
- `ReadMicrophonePcm16()` dùng timed mutex, timeout, thống kê partial read và timeout.
- Source ghi MIC_RAW lần đầu, MIC_LEVEL theo cửa sổ khoảng 2 giây, MIC_IO theo cửa sổ khoảng 5 giây.
- `XiaozhiAudioRuntime::SetCaptureEnabled(true)` bật voice processing của AudioService/AFE; tắt capture sẽ disable processing và yêu cầu dừng input.
- Audio runtime chuyển packet từ AudioService send queue thành uplink frame; downlink frame được chuyển lại vào AudioService.
- Client có callback VAD để cập nhật trạng thái nói/nghe.

### Uplink/downlink

**[SOURCE-VERIFIED]**

- AudioService tạo packet gửi; client lấy packet từ send queue.
- Server config có thể thương lượng Opus, số kênh, sample rate và frame duration.
- WebSocket path gửi/nhận audio binary; MQTT+UDP path dùng audio UDP.
- Downlink được chuyển vào audio runtime/AudioService để decode và phát qua I2S speaker.
- Speaker output format phần cứng hiện cấu hình 24 kHz; decoder/codec bridge xử lý định dạng frame được thương lượng.
- Có callback playback progress, playback drained và playback started.

**[NOT-VERIFIED]** Chưa xác nhận microphone sensitivity, mức nhiễu, clipping thực tế, AFE/VAD nhận giọng ổn định, speaker không underrun, hoặc một lượt hội thoại hoàn chỉnh trên thiết bị trong lần lập baseline này.

## 8. Thông báo bằng âm thanh

File: `chatai_xiaozhi_notify_player.cpp`.

**[SOURCE-VERIFIED]**

- Notify player nhận audio URL HTTP/HTTPS, danh sách subtitle có timestamp và playback ID.
- Worker tải OGG qua HTTPS/HTTP, dùng certificate bundle cho TLS, giải mã/demux OGG thành packet và đưa frame vào audio runtime.
- Subtitle callback cập nhật text theo playback position.
- Có xử lý cancel, playback drained, underrun và finished callback.
- BOOT bật chat sẽ dừng notification đang phát và reset notification playback trước khi bắt đầu hội thoại.

**[NOT-VERIFIED]** Source có pipeline notification nhưng chưa xác minh lần phát thông báo cụ thể trên thiết bị ở snapshot này.

## 9. OLED, đồng hồ và IP

File: `display_manager.cpp`.

**[SOURCE-VERIFIED]**

- Driver SSD1306, 128×64, I²C address 0x3C.
- SDA GPIO 41, SCL GPIO 42, I²C_NUM_0; pull-up nội bộ được bật.
- Render task refresh mỗi 60 ms.
- Góc trên trái chỉ vẽ Wi-Fi icon khi `NetworkManager::IsConnected()`.
- Đồng hồ dùng SNTP server `pool.ntp.org`; chỉ bắt đầu sau khi Wi-Fi connected. Chỉ hiển thị giờ khi SNTP status báo completed và epoch hợp lệ.
- Timezone POSIX `UTC-7`, tương ứng UTC+7 Việt Nam.
- Vùng giữa hiển thị trạng thái âm thanh/runtime: connecting, waiting, listening, processing, speaking hoặc lỗi microphone.
- Footer ticker hiển thị `CHATAI TPIOS - IP: <địa chỉ> - SERVER READY/WAIT`, trong đó IP lấy từ router qua `NetworkManager::GetIpAddress()`.
- Battery icon hiện được vẽ bằng pixel.

**[NOT-VERIFIED]** Trong file display manager đã kiểm tra, battery icon chỉ là hình biểu tượng; chưa thấy nguồn ADC/PMIC hoặc phép đo phần trăm pin. Không ghi nhận % pin là dữ liệu đo thật nếu chưa tìm thấy nguồn đo khác.

**[DEVICE-REPORTED]** Người dùng đã yêu cầu layout Wi-Fi icon trái, đồng hồ giữa và battery icon phải; ticker dưới hiển thị IP router cấp cho ESP32. Đây là yêu cầu/giao diện đã được triển khai trong source, nhưng tài liệu không kết luận trạng thái hiển thị trên thiết bị hiện tại nếu chưa có xác nhận test tương ứng.

## 10. Diagnostics và dấu vết reset

File: `diagnostic_manager.cpp`.

**[SOURCE-VERIFIED]**

- Log buffer trong RAM giới hạn khoảng 8192 bytes.
- Mỗi dòng có timestamp ICT/UTC khi time sync đã được xác nhận; nếu chưa có thì dùng uptime.
- Có RTC no-init retained crash context với magic/checksum.
- Có RTC no-init retained boot event ring buffer tối đa 4 sự kiện.
- `SetCrashContext()`, `SetBootContext()`, `RestorePreviousCrashContext()` lưu/khôi phục giai đoạn gần reset.
- `Clear()` xóa log buffer và retained contexts.
- `app_main.cpp::ReportBootDiagnostics()` ghi boot ID, reset reason, free heap và minimum free heap.
- `button_manager.cpp` ghi sự kiện BOOT/PTT và stack watermark.

Các stage quan trọng có thể tìm trong log: `BOOT`, `BUTTON`, `BOOT_ACTION`, `XIAOZHI`, `XIAOZHI_CHAT`, `XIAOZHI_OTA`, `XIAOZHI_MQTT`, `NETWORK`, `DNS`, `TCP_443`, `INTERNET`, `MIC_RAW`, `MIC_LEVEL`, `MIC_IO`, `AUDIO_RUNTIME`, `AFE_CAPTURE`.

**[NOT-VERIFIED]** Không có log runtime mới của thiết bị trong lần tạo tài liệu này; các tên stage là những điểm source có khả năng ghi log, không phải kết quả của một phiên cụ thể.

## 11. Web Admin, NVS và OTA

**[SOURCE-VERIFIED]**

- `app_main.cpp` khởi tạo mDNS hostname `chatai`, log địa chỉ `http://chatai.local/`.
- `web_admin.cpp` là server HTTP cục bộ; tài liệu `docs/WEB_ADMIN.md` mô tả dashboard Wi-Fi, thiết lập Gemini, audio, telemetry, diagnostics và OTA.
- `network_manager.cpp` sở hữu Wi-Fi provisioning và reconnect.
- NVS namespaces trong source/tài liệu gồm `chatai_net` (Wi-Fi) và `chatai_cfg` (speaker volume, mic gain). Cần xem từng module settings để xác nhận đầy đủ key cụ thể trước khi thay đổi.
- `app_main.cpp` có xử lý OTA pending-verify/rollback và xác nhận image sau startup.
- Component list trong `firmware/standalone/main/CMakeLists.txt` có `chatai_xiaozhi_ota.cpp`, `web_admin.cpp`, `firmware_info.cpp` và các thành phần OTA.

**[DOC-CONFLICT]** `docs/CHECKPOINT.md` và README mô tả OTA application-only dual-slot, nhưng `docs/STANDALONE_MIGRATION.md` lại có đoạn nói single-app/no OTA. Cần cập nhật tài liệu kiến trúc ở một nhiệm vụ riêng; file baseline này không sửa các tài liệu khác.

**[NOT-VERIFIED]** Không upload OTA, không chạy workflow/build và không flash trong lần lập baseline. Không xác nhận layout partition thực tế trên thiết bị bằng cách đọc flash.

## 12. Phần cứng và giới hạn xác minh

Theo `docs/HARDWARE.md` và ghi nhận người dùng:

- Board được người bán mô tả là Mini ESP32-S3-N16R8; người dùng xác nhận thiết bị có 16 MB flash / 8 MB PSRAM.
- Sản phẩm có OLED 0.96 inch và loa theo bộ; firmware cấu hình SSD1306 128×64.
- Chưa có schematic/BOM/PCB revision xác thực pinout của đúng bo mạch.
- GPIO mapping trong firmware là cấu hình phần mềm, không thay thế xác minh điện thực tế.
- Hardware document ghi rõ không được tự suy ra board này là XH-S3E-AI V1.0 hoặc mặc định giống hoàn toàn upstream `bread-compact-wifi`.

**[NOT-VERIFIED]** Chưa đo điện áp, kiểm tra logic level, xác nhận pin routing, xác nhận microphone IC, codec/amplifier IC hoặc partition table vật lý trong lần lập baseline.

## 13. Lịch sử lỗi và thay đổi liên quan

| Mốc | Nội dung | Mức độ |
|---|---|---|
| Trước commit `02f4ed74` | ButtonTask stack là 3072 bytes | [HISTORICAL] |
| `02f4ed74b85f606990dc3dbc13eac2e305407e8f` | Tăng ButtonTask stack 3072 → 6144 bytes | [SOURCE-VERIFIED] |
| Trao đổi trước đây | Người dùng báo BOOT có lúc khiến ESP32-S3 reset | [DEVICE-REPORTED] |
| `c54e6c1af88844cfa9099813b739b07a412e7eb0` | Thêm Wi-Fi icon, đồng hồ SNTP, battery icon trên OLED | [HISTORICAL] |
| `7f37762d3cbbede915c6d94c1186a9106519a545` | Footer OLED đổi sang hiển thị IP router cấp và server READY/WAIT | [HISTORICAL] |
| `06baa9aca5f825d2c6557c1bdda037407f9aef47` | Chỉ khởi tạo SNTP sau khi Wi-Fi kết nối; đồng hồ chờ sync hoàn tất | [SOURCE-VERIFIED] |

Không có xác nhận trong tài liệu này rằng lỗi BOOT reset đã hết; cần dựa vào test thiết bị và log reset reason.

## 14. Baseline để thay backend bằng Gemini Live

Mục tiêu của phần này là xác định ranh giới thay thế, không triển khai Gemini trong tài liệu này.

### Thành phần cần bảo toàn

1. Board startup, NVS, Wi-Fi provisioning, reconnect và IP.
2. OLED status, SNTP, ticker và thông báo.
3. Button manager: BOOT click-release bật continuous conversation; PTT press/release giữ hành vi hiện tại; volume buttons.
4. Audio hardware abstraction: I2S RX/TX, PCM16 conversion, input gain, output volume, amplifier control.
5. AudioService/AFE integration, callback VAD, capture lifecycle, audio queue và speaker playback.
6. Diagnostic manager, crash context, reset reason, BOOT event history và log copy/clear.
7. Web Admin, settings persistence, OTA/rollback và firmware status.
8. Không làm mất notification playback nếu vẫn được dùng trong trải nghiệm ChatAI.

### Thành phần có thể thay thế khi chuyển backend

- Xiaozhi OTA discovery/server config.
- Xiaozhi activation/device binding.
- MQTT control transport và UDP audio transport.
- Xiaozhi WebSocket protocol/control messages.
- Xiaozhi hello/session state và reconnect logic.
- Mapping gói audio theo transport/protocol của Xiaozhi.

### Giao diện cần giữ ổn định khi thay backend

Nên giữ một lớp interface tương đương các hành vi hiện tại:

- `Initialize()`
- `IsReady()`
- `SetChatEnabled(bool)`
- `IsChatEnabled()`
- `SetPushToTalk(bool)`
- `IsListening()`
- `GetRuntimeState()`

Không bắt buộc giữ nguyên tên namespace `chatai::xiaozhi` sau khi kiến trúc được tách backend; điều quan trọng là không làm thay đổi hành vi mà ButtonTask và DisplayManager đang phụ thuộc.

### Kiểm thử bắt buộc trước/sau chuyển đổi

- BOOT nhấn/nhả: chỉ thực hiện hành động một lần sau release; không reset.
- BOOT trong provisioning: không bật hội thoại.
- PTT: nhấn bắt đầu capture, nhả dừng capture.
- Mic: xác minh PCM16 16 kHz, mức RMS/peak, timeout, clipping và độ nhạy thực tế.
- Loa: xác minh PCM playback, volume, interruption và không underrun.
- Một phiên hội thoại hoàn chỉnh: setup/session ready, uplink, response audio, turn complete/interruption.
- Mất Wi-Fi/server: trạng thái hiển thị và reconnect đúng, không treo task.
- OLED: Wi-Fi, clock, battery indicator, IP và server state không bị mất.
- Diagnostics: log đủ để phân biệt lỗi button, Wi-Fi, microphone, transport, server và speaker.
- OTA: cập nhật/rollback không làm mất Wi-Fi/API settings; chỉ kiểm tra khi được người dùng cho phép riêng.

### Nguyên tắc phân loại lỗi

Khi Gemini không phản hồi, phải tách ít nhất bốn lớp:
1. Button/session request.
2. Microphone capture/AFE/audio uplink.
3. Network/TLS/WebSocket/Gemini setup.
4. Downlink decode/speaker playback.

Không quy lỗi backend khi chưa xác minh audio capture; không quy lỗi microphone khi server session chưa ready.

## 15. Tài liệu nguồn và các điểm chưa thống nhất

### Source chính đã kiểm tra

- `README.md`
- `firmware/standalone/CMakeLists.txt`
- `firmware/standalone/main/CMakeLists.txt`
- `firmware/standalone/main/app_main.cpp`
- `firmware/standalone/main/network_manager.cpp`
- `firmware/standalone/main/button_manager.cpp`
- `firmware/standalone/main/audio_manager.cpp`
- `firmware/standalone/main/display_manager.cpp`
- `firmware/standalone/main/chatai_xiaozhi_client.cpp/.h`
- `firmware/standalone/main/chatai_xiaozhi_mqtt.cpp`
- `firmware/standalone/main/chatai_xiaozhi_websocket.cpp`
- `firmware/standalone/main/chatai_xiaozhi_audio_runtime.cpp`
- `firmware/standalone/main/chatai_xiaozhi_notify_player.cpp`
- `firmware/standalone/main/diagnostic_manager.cpp`
- `docs/CHECKPOINT.md`
- `docs/STANDALONE_MIGRATION.md`
- `docs/HARDWARE.md`
- `docs/GEMINI_LIVE_WIRE_PROTOCOL.md`
- `docs/WEB_ADMIN.md`

### Cần làm rõ ở một nhiệm vụ tài liệu riêng

1. Đồng bộ README, CHECKPOINT, STANDALONE_MIGRATION và GEMINI_LIVE_WIRE_PROTOCOL với runtime Xiaozhi-only tại HEAD hiện tại.
2. Làm rõ tình trạng OTA/partition vì các tài liệu mô tả không thống nhất.
3. Cập nhật trạng thái kiểm thử phần cứng dựa trên log/device test mới, không suy ra từ source.
4. Kiểm tra battery icon có được nối với nguồn đo pin thực tế hay hiện chỉ là biểu tượng.
5. Khi có sơ đồ mạch đúng model Mini ESP32-S3-N16R8, xác minh GPIO microphone, speaker, PTT và amplifier enable.

---

## Phạm vi thay đổi của lần lập baseline

- Chỉ thêm file `XIAOZHI_BASELINE.md`.
- Không sửa firmware source.
- Không sửa README hoặc tài liệu hiện có.
- Không tạo branch.
- Không chạy build, GitHub Actions, Codemagic hoặc flash thiết bị.
- Không thực hiện kiểm thử phần cứng.
