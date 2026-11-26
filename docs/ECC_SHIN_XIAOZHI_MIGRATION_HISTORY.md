# ECC Shin — ChatAI TPIOS → Xiaozhi migration history

> **Purpose:** durable engineering record of the research, architecture decisions, source changes, build failures/fixes, and first successful build/install of the Xiaozhi-integrated ChatAI TPIOS firmware. This document is intended to let a future engineer or ChatGPT session resume work without reconstructing the entire investigation.
>
> **Record date:** 2026-11-26  
> **Repository:** `Truongpink/esp32-claude`  
> **Branch:** `main` only  
> **Recorded source HEAD:** `c251368ac66099c6846379440a6eeafb4c442c37`

## 1. Executive summary

The project began with a ChatAI TPIOS standalone ESP32-S3 firmware architecture in which ChatAI owned Wi-Fi, I2S audio, OLED, local Web Admin, NVS configuration, and a direct Gemini Live WebSocket client. The project then investigated and integrated the upstream Xiaozhi voice application while deliberately retaining ChatAI's existing hardware and product infrastructure.

The selected result is **not a wholesale replacement of the ChatAI firmware with a stock Xiaozhi board image**. It is a ChatAI standalone firmware that embeds/adapts selected Xiaozhi v2.6.0 application, audio, AFE, protocol, and board abstractions. ChatAI continues to own Wi-Fi, Web Admin, the existing I2S channels, display, buttons, diagnostics, NVS, partition layout, and manual OTA. Xiaozhi is the only enabled voice client in the current firmware.

**Milestone reached on 2026-11-26:**
- GitHub Actions workflow run **#97** completed successfully on commit `c251368ac66099c6846379440a6eeafb4c442c37`.
- ESP-IDF 6.1 / ESP32-S3 build completed and produced `ChatAI_TPIOS.bin`.
- CI verified the application image and OTA-slot size, packaged the output, and uploaded the artifact.
- The user independently reported that the firmware was installed and the device booted and ran, currently “tạm ổn”; more real-world testing is still pending.

This is a first successful build/install milestone, **not a declaration that long-duration runtime stability, all audio cases, all recovery paths, or every optional AFE feature have been fully validated**.

## 2. Source provenance and pinned upstream

- Xiaozhi upstream: https://github.com/78/xiaozhi-esp32
- Release tag: `v2.6.0`
- Pinned submodule commit: `4de00aed740595a8713b01f8332b9b2cf30685bc`
- Submodule path: `firmware/standalone/third_party/xiaozhi-esp32`
- Git submodule declaration: `.gitmodules`
- Upstream license: MIT. Preserve its license and copyright notices for upstream code.

The submodule is pinned intentionally for reproducibility. Do not follow upstream moving `main` implicitly. Any update of the submodule commit requires explicit review and approval. The source checkout must initialize submodules recursively; the build workflow now does this.

## 3. Architecture: before and after

### 3.1 Earlier ChatAI standalone direction

The earlier standalone architecture was built around:
- ChatAI `NetworkManager` for Wi-Fi provisioning/reconnect.
- ChatAI `AudioManager` for microphone/speaker I2S and amplifier control.
- ChatAI SSD1306 `DisplayManager`.
- Local HTTP Web Admin for settings and device administration.
- NVS for persistent configuration.
- Direct Gemini Live WSS/TLS client as the AI voice session.
- Existing dual-slot 16 MB flash/OTA arrangement and PSRAM defaults.

Historical documentation describing this Gemini-only architecture remains in the repository, but it is **not the current voice architecture**. See §12 for stale-document warnings.

### 3.2 Current integrated architecture

The current source integrates the Xiaozhi application and voice path while keeping ChatAI as the board/product owner:

| Subsystem | Current owner / role |
|---|---|
| Firmware entry point and platform initialization | ChatAI `app_main` |
| Wi-Fi, provisioning, reconnect, HTTP server lifecycle | ChatAI `NetworkManager` |
| Web Admin routes and embedded page | ChatAI `web_admin` |
| Microphone/speaker I2S channels and amplifier | ChatAI `AudioManager` |
| Xiaozhi-facing codec contract | ChatAI adapter implementing pinned Xiaozhi `AudioCodec` contract |
| PCM/I2S bridge | `ChatAIAudioCodecBridge` and `ChatAIXiaozhiAudioCodec` |
| Opus encoding/decoding | Espressif `esp_audio_codec` via ChatAI adapter |
| Voice processing / AFE | Integrated Xiaozhi AudioService and AFE engine using ESP-SR |
| AI session / protocol | ChatAI Xiaozhi client and adapted upstream application/protocol |
| OLED, buttons, diagnostics, status LED | Existing ChatAI managers |
| Persistent ChatAI configuration | ChatAI NVS namespaces; Xiaozhi identity has dedicated `chatai_xiaozhi` namespace |
| Firmware image, partitions and manual Web Admin OTA | ChatAI |
| Gemini | Source/settings remain in repository for possible later work, but Gemini is not initialized, compiled or used as the current Xiaozhi voice path |

Relevant current files include:
- `firmware/standalone/main/app_main.cpp`
- `firmware/standalone/main/network_manager.*`
- `firmware/standalone/main/audio_manager.*`
- `firmware/standalone/main/audio_codec.*`
- `firmware/standalone/main/chatai_audio_codec_bridge.*`
- `firmware/standalone/main/chatai_xiaozhi_audio_codec.*`
- `firmware/standalone/main/chatai_xiaozhi_client.*`
- `firmware/standalone/main/chatai_xiaozhi_application.*`
- `firmware/standalone/main/chatai_xiaozhi_protocol.*`
- `firmware/standalone/main/chatai_xiaozhi_board.cpp`
- `firmware/standalone/main/web_admin.*`
- `firmware/standalone/main/display_manager.*`
- `firmware/standalone/main/button_manager.*`
- `firmware/standalone/main/diagnostic_manager.*`
- `firmware/standalone/main/status_led_manager.*`
- `firmware/standalone/XIAOZHI_INTEGRATION.md`

### 3.3 Key integration principle

Do not start the stock Xiaozhi board startup in a way that creates a second Wi-Fi manager or a second I2S driver. The adapter board reports ChatAI board identity and delegates network events to ChatAI's NetworkManager. Its `StartNetwork()` deliberately does not initialize Wi-Fi. The Xiaozhi codec adapter reuses ChatAI AudioManager I2S channels.

The current startup sequence in `app_main.cpp` initializes NVS, ESP-NETIF/event loop, mDNS, ChatAI Wi-Fi, display and audio, then initializes the Xiaozhi client/application. If AudioManager is unavailable, Xiaozhi startup is skipped rather than creating an independent audio path.

## 4. Voice/audio implementation findings

### 4.1 Why upstream was not a drop-in replacement

The pinned upstream AudioService depends on an AudioCodec abstraction, Opus encode/decode, resampling, FreeRTOS queues/tasks, audio engine, and ESP32-S3 AFE/ESP-SR. The upstream application also expects its own Board, NetworkInterface, Settings, Assets, and lifecycle abstractions. The integration therefore required explicit adapters and ownership boundaries rather than copying a stock board configuration.

### 4.2 Codec and audio bridge

- A board-neutral `AudioCodec` declaration was added with class-definition tokens compatible with the pinned upstream v2.6.0 contract. Upstream board-specific include/macros are excluded.
- ChatAI owns base codec methods and persistent settings; upstream `audio_codec.cc` is not compiled because it assumes Xiaozhi Settings ownership.
- `ChatAIXiaozhiAudioCodec` adapts the Xiaozhi contract to `ChatAIAudioCodecBridge` and existing ChatAI AudioManager.
- No second I2S driver is created.
- Current PCM format at the ChatAI bridge: mono PCM16, microphone input 16 kHz and speaker output 24 kHz.
- Added Espressif `esp_audio_codec ~2.5.0` and `ChatAIOpusCodec`. Uplink encoder input is mono 16 kHz / 60 ms (960 samples). Decoder follows negotiated format.
- `XiaozhiAudioRuntime` uses separate FreeRTOS microphone-capture and speaker-playback tasks. Uplink and downlink queues are bounded to four packets; the oldest packet is dropped when full to bound latency.
- Current supported downlink is 24 kHz mono / 60 ms. Other negotiated formats are rejected until a resampler path is implemented; do not play an unsupported format at the wrong speed.
- Runtime shutdown waits for both worker tasks before stopping codec/I2S. Keep the runtime object alive throughout any active session.

### 4.3 AFE, VAD, gain, and limitations

The integration document records the following current configuration:
- FD AFE voice processing and VAD enabled.
- When no VAD model is supplied, ESP-SR uses WebRTC VAD fallback.
- VAD mode 2, 64 ms minimum speech window, 64 ms VAD delay.
- WebRTC AGC: 12 dB compression gain and -3 dBFS target.
- `MIC_VAD` diagnostics record speech/silence transitions.
- `MIC_LEVEL` reports pre-AFE PCM RMS/peak approximately every two seconds while capture is active.
- The Web Admin Audio tab has persisted software microphone gain from -6 to +6 dB at `chatai_cfg/mic_gain_db`; default 0 dB. It is applied to PCM16 before AFE with saturation. It is not hardware preamp gain and high settings may clip.
- WakeNet is unavailable because the current partition table has no `model` partition and no ESP-SR model image is shipped.
- Device AEC is not active: the bridge exposes one microphone channel and does not provide a calibrated synchronized playback-reference channel.
- Noise suppression is disabled because no NSNet model is provisioned.
- Do not advertise WakeNet, device AEC, or NS as operational unless required models/reference paths are actually supplied and tested.
- Do not repurpose the existing `assets` partition or change OTA/partition layout without approval.

### 4.4 Session and transport

- The client performs Xiaozhi OTA configuration discovery at `https://api.tenclass.net/xiaozhi/ota/` using HTTPS certificate-bundle verification.
- A stable UUID v4 is persisted in `chatai_xiaozhi` NVS; device report includes MAC and app/chip/partition metadata.
- Unsupported MCP, AEC, UDP probe and glyph-push capabilities are explicitly reported unavailable.
- Server-provided MQTT config has precedence; otherwise WebSocket config is used.
- Activation message/code is retained for display. Activation polling uses `POST /xiaozhi/ota/activate`; HTTP 202 waits three seconds and HTTP 200 completes binding and triggers fresh OTA discovery.
- Credentials are held in memory and must not be logged.
- MQTT control uses TLS and OTA-provided credentials/topics. MQTT+UDP audio uses a connected UDP socket, AES-128 key imported to PSA Crypto, AES-CTR protection for the 16-byte Xiaozhi audio header and Opus payload, and monotonically increasing receive sequence validation.
- WebSocket supports raw Opus version-1 frames and version-2/3 binary headers.
- MQTT+UDP is selected when supplied by OTA configuration; otherwise WebSocket is used.
- If system time is invalid, SNTP synchronization occurs before HTTPS/TLS. OTA `server_time` is then applied for subsequent TLS sessions.
- Wi-Fi reconnect uses capped exponential backoff of 1–30 seconds. Setup AP recovery is supported after repeated failures while station retries continue.
- Normal idle periods do not intentionally disconnect the station.
- Audio runtime starts only after valid protocol hello. Microphone peripheral remains disabled while idle and starts only after listen-start; server TTS start/stop pauses/resumes capture.
- BOOT starts a one-shot manual listen turn. Local VAD silence detection sends listen/stop after 650 ms of stable silence; server processes the utterance and returns TTS, then the turn returns to idle.
- Existing touch PTT remains a separate hold-to-talk manual path while the Xiaozhi session is ready.
- Xiaozhi is the only enabled voice client; no Gemini worker/audio arbitration is started.

## 5. Hardware profile and pin evidence

Target module variant confirmed by the user: ESP32-S3-N16R8, 16 MB flash and 8 MB PSRAM. The seller listing identifies a Mini ESP32-S3-N16R8 voice-AI kit but does not provide a matching schematic or detailed electrical pinout.

**Important correction:** repository hardware documentation says the seller listing does not identify the board as “XH-S3E-AI V1.0”. Do not treat that name as confirmed hardware identity. Do not assume the board is identical to upstream `bread-compact-wifi`.

Current ChatAI firmware pin settings (configured values, not all electrically verified):
| Function | Current firmware setting |
|---|---:|
| OLED SDA / SCL | GPIO 41 / GPIO 42 |
| OLED | SSD1306 configured 128×64, address 0x3C |
| RGB LED | GPIO 48 |
| Speaker I2S DOUT / BCLK / WS | GPIO 7 / GPIO 15 / GPIO 16 |
| Amplifier enable | GPIO 17, HIGH |
| BOOT | GPIO 0, active LOW |
| Touch/PTT | GPIO 43, active HIGH |
| Volume Up / Down | GPIO 40 / GPIO 39, active LOW |
| Microphone I2S BCLK / WS / DIN | GPIO 5 / GPIO 4 / GPIO 6 |

The pinned upstream `bread-compact-wifi` reference uses GPIO 47 for touch, while ChatAI currently uses GPIO 43. This mismatch is not permission to change the pin. Obtain the exact schematic or safely measure the board before changing pin assignments. Amplifier IC, microphone IC/type, codec/DAC and exact amplifier-enable routing remain unconfirmed.

## 6. Flash, partition and memory configuration

- ESP-IDF target: `esp32s3`.
- Flash setting: 16 MB.
- PSRAM: 8 MB Octal, 80 MHz, malloc enabled.
- Current PSRAM thresholds: `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`.
- Bluetooth is disabled in `sdkconfig.defaults`: `CONFIG_BT_ENABLED=n`, `CONFIG_BT_NIMBLE_ENABLED=n`, `CONFIG_BT_NIMBLE_ROLE_OBSERVER=n`.
- Current partition file: `firmware/standalone/partitions/16m.csv`.
- Layout: NVS at 0x9000, otadata at 0xD000, PHY data at 0xF000, ota_0 at 0x20000 with size 0x3F0000, ota_1 with size 0x3F0000, assets SPIFFS at 0x800000 with size 8 MB.
- Keep the installed bootloader/partition table and existing NVS/Wi-Fi settings. Do not erase/repartition flash as part of routine integration.
- Physical board partition table has not been independently read back; firmware config/build is evidence of the repository's intended table, not proof of the exact table installed on the user's board.

## 7. Key research and implementation timeline

This table records representative milestones from Git history. Use the full Git history for exhaustive per-commit diffs.

| Date (UTC) | Commit (short SHA) | Milestone |
|---|---|---|
| Nov 22 | `c5c21f9d3a46` | Pin Xiaozhi v2.6.0 source for Chat Voice integration |
| Nov 22 | `03cd35303e5e` | Add standalone Xiaozhi codec bridge |
| Nov 22 | `0338864344a3` | Introduce independent PCM interface |
| Nov 22 | `b1d463d421c3` | Add Xiaozhi codec adapter for ChatAI I2S |
| Nov 22 | `76ce08e35c90` | Add ChatAI Xiaozhi Opus codec adapter |
| Nov 22 | `43097822b362` | Add ChatAI Xiaozhi audio runtime |
| Nov 22 | `e49b764aed2f` | Add server OTA config discovery |
| Nov 22 | `d8132a2c6996` | Add native MQTT control transport |
| Nov 22 | `242763425a22` | Connect MQTT/UDP voice sessions |
| Nov 22 | `699f2f766a68` | Add WebSocket voice transport |
| Nov 22 | `3ee78581c047` | Complete MQTT and WebSocket voice sessions |
| Nov 22 | `95d36b3143bc` | Integrate Xiaozhi AudioService and ESP-SR AFE |
| Nov 22–23 | `ef2799586c1f` | Activate Xiaozhi as sole voice client |
| Nov 23 | `82a52aba6f64` | Make BOOT start one-shot Xiaozhi conversation |
| Nov 23 | `b183d4c9b7f8` | End one-shot turns on local VAD silence |
| Nov 23 | `bc2b911efad4` | Add Web microphone gain and improve Wi-Fi recovery |
| Nov 23 | `e03572670465` | Add Xiaozhi firmware baseline documentation |
| Nov 25 | `6842024a0fcc` | Restore AudioService and retain safe rollback cleanup |
| Nov 25 | `f63e669fa980` | Validate Opus encoder frame sizes |
| Nov 25 | `c5b84b629fa3` | Retry Xiaozhi protocol startup failures with bounded backoff |
| Nov 25 | `90f2790a5d90` | Close idle Xiaozhi audio channel on network loss |
| Nov 26 | `3984f979a5fe` | Confirm OTA image after Xiaozhi startup succeeds |
| Nov 26 | `fa68b932dc51` | Keep bootloader OTA confirmation in ChatAI startup |
| Nov 26 | `357f115a026f` | Expose microphone diagnostics in Web Admin |
| Nov 26 | `0749d8eda7d3` | Expose audio uplink diagnostics through ChatAI client |
| Nov 26 | `56cfab66a4fc` | Fix standalone workflow checkout and optional release |
| Nov 26 | `e7da672620f8` | Fix missing Xiaozhi display declaration include |
| Nov 26 | `dee8eb558063` | Fix ESP-IDF 6.1 upstream MQTT warning |
| Nov 26 | `b030997c08ed` | Preserve standalone board compile definitions |
| Nov 26 | `fafab516d591` | Link Xiaozhi backlight implementation |
| Nov 26 | `544d7c95887e` | Declare ESP-IDF LEDC driver dependency |
| Nov 26 | `c251368ac660` | Scope ESP-IDF initializer warning exception to upstream backlight |

Other history contains detailed race/lifecycle fixes for AFE, VAD, Opus frame sizing, resampling, playback ownership, activation cancellation, Wi-Fi reconnect/AP restoration, protocol retries, and audio diagnostics. Search Git history by terms such as `AFE`, `resampler`, `activation`, `retry`, `Wi-Fi`, `BOOT`, `VAD`, `Opus`, `MQTT`, and `WebSocket` before changing these paths.

## 8. Build workflow failures and evidence-based fixes

The workflow is `.github/workflows/build-standalone.yml`, manually triggered by `workflow_dispatch`. It checks out `main` with recursive submodules, builds with ESP-IDF v6.1 for ESP32-S3, checks the application image magic and OTA-slot size, creates a versioned BIN/ZIP in the workspace, validates ZIP contents, uploads the BIN artifact, and only creates an official GitHub Release when `publish_release=true`.

| Failed stage / evidence | Root cause | Fix |
|---|---|---|
| Checkout / missing upstream source | Submodule was not recursively checked out | Commit `56cfab66a4fc` adds `submodules: recursive` |
| `NoDisplay` unknown base class | Board adapter did not include upstream display declaration | `e7da672620f8` adds `#include "display.h"` |
| ESP-IDF 6.1 missing-field initializer warning promoted to error in `mqtt_protocol.cc` | Upstream aggregate initializer omitted newly added optional timer fields | `dee8eb558063` applies narrow source-specific `-Wno-error=missing-field-initializers` |
| Standalone board compile definitions | Board/build definitions needed to remain present through CMake integration | `b030997c08ed` preserves compile definitions |
| Undefined/missing backlight implementation linkage | Upstream backlight source was referenced but not linked | `fafab516d591` links upstream `backlight.cc` |
| `driver/ledc.h` not found | ESP-IDF LEDC driver component dependency was missing | `544d7c95887e` adds `esp_driver_ledc` to component `REQUIRES` |
| `backlight.cc` missing initializer warning promoted to error | ESP-IDF 6.1 added optional LEDC struct fields | `c251368ac660` adds only `backlight.cc` alongside `mqtt_protocol.cc` to the narrow source-specific warning exception |

The warning exception does **not** disable `-Werror` for the entire component. Preserve this narrow scope. Do not globally suppress compiler warnings to make the build pass.

Earlier failed workflow runs:
- #91: `e7da672...` area; display declaration issue.
- #93: MQTT initializer warning.
- #94: missing LEDC header.
- #95 and #96: LEDC header was resolved; backlight initializer warning remained.
- #97: successful build on final source HEAD `c251368...`.

## 9. First successful build and install milestone

**GitHub Actions run #97**
- Run ID: `45809252292`
- URL: https://github.com/Truongpink/esp32-claude/actions/runs/45809252292
- Workflow: `Build ChatAI TPIOS standalone firmware`
- Branch: `main`
- Source commit: `c251368ac66099c6846379440a6eeafb4c442c37`
- Job ID: `139872487916`
- Result: `success`
- Build number: `#80`
- ESP-IDF: `v6.1`
- Target: `esp32s3`
- Build, package/verify, and artifact upload steps all succeeded.
- Official firmware release step was skipped because `publish_release=false`.

CI evidence:
- Generated `ChatAI_TPIOS.bin`.
- Binary size: **3,378,016 bytes** (`0x338b60`).
- Smallest OTA app slot: **4,128,768 bytes** (`0x3f0000`).
- Remaining space: **750,752 bytes** (18%).
- ZIP validation succeeded; ZIP contained exactly `ChatAI-TPIOS-80.bin`.
- Artifact `ChatAI-TPIOS-80` uploaded successfully (GitHub artifact ID `15112970318`).
- Uploaded artifact digest: `814a40b2f508166949c6ddbf35317bf5b4173cbed93868085951104e43c3b2e4`.

Warnings still present in the successful CI log:
- Third-party `esp-dl` unused-variable warnings.
- `app_main.cpp` variables `ota_image_confirmed` and `ota_rollback_detected` set but unused in that configuration.
- Narrowly demoted missing-field-initializer warnings in upstream `backlight.cc` and `mqtt_protocol.cc`.
- GitHub action Node.js deprecation notices.
These were warnings, not build blockers in run #97. Do not silently erase or broadly suppress them; review only if they become relevant to correctness or a future toolchain change.

**User-reported device result (2026-11-26):**
- Firmware installed successfully.
- Device booted and ran.
- Current behavior described by the user as “tạm ổn”.
- User will continue testing.
This is user-reported hardware evidence. It does not yet establish long-duration reliability or complete functional coverage.

## 10. Decisions and constraints approved for the migration

- Keep ChatAI NetworkManager and Wi-Fi ownership.
- Keep Web Admin and existing local administration.
- Keep ChatAI AudioManager and use an AudioCodec bridge to Xiaozhi.
- Keep OLED, buttons, status LED and diagnostics.
- Keep NVS and existing network/configuration data.
- Keep the 16 MB dual-slot partition/OTA design; no flash erase or repartition as part of migration.
- Integrate Xiaozhi upstream application/voice path; Xiaozhi is the sole active voice client.
- Gemini source/settings may remain in tree but Gemini is not active in this voice path.
- Bluetooth/BLE remain disabled.
- Do not edit upstream Xiaozhi submodule source or move its pinned commit without approval.
- Keep upstream license notices.
- No second Wi-Fi manager or second I2S driver.
- Do not claim WakeNet, device AEC, or NS are active without models/reference path and validation.
- Keep changes minimal and evidence-based.

## 11. Open risks and test plan

The following are test targets, not claims of confirmed defects.

### Runtime / stability
- Long-duration idle and active voice sessions.
- Repeated boot/start/stop and repeated BOOT one-shot turns.
- Touch PTT hold/release and interactions with BOOT.
- Recovery after Wi-Fi loss, router restart, DHCP/IP change, and repeated reconnect.
- Xiaozhi OTA config discovery, activation/binding and reconnect after activation failure.
- MQTT+UDP and WebSocket session paths as returned by service configuration.
- TTS interruption/stop, playback completion, and microphone re-enable.
- Check reset reason/panic/backtrace before attributing any reset to a pin, heap, stack or race.

### Audio
- Verify microphone capture level, clipping and AGC behavior at different acoustic levels.
- Confirm 16 kHz mono PCM input, Opus uplink frame timing, server recognition and response.
- Confirm 24 kHz mono downlink, decoder behavior, speaker playback and no speed/pitch distortion.
- Test long responses, consecutive responses and one-shot silence detection.
- Watch `MIC_LEVEL`, `MIC_VAD`, audio uplink counters and turn latency diagnostics.
- Confirm the codec runtime releases/owns channels correctly on disconnect and session restart.

### Memory / scheduling
- Record free heap, minimum free heap, PSRAM usage and task stack high-water marks during idle, activation, capture, AFE and TTS.
- Watch for gradual heap loss across repeated sessions.
- Investigate blocking operations, queue saturation, dropped frames and task starvation using logs before changing scheduling.
- Do not infer stack overflow or heap corruption from a single reset reason without supporting evidence.

### Web Admin / network / OTA
- Verify Web Admin remains reachable by IP and mDNS hostname `http://chatai.local/` where supported.
- Confirm saved Wi-Fi settings survive reboot and reconnect.
- Verify Web Admin status and diagnostic endpoints remain responsive during voice sessions.
- Validate manual OTA only with explicit authorization; verify target partition, image size, boot confirmation and rollback behavior.
- Preserve NVS and current partition layout.

### Hardware
- Confirm GPIO mapping against exact board schematic or safe physical measurements before any pin change.
- In particular, do not infer the user's physical board is upstream `bread-compact-wifi`; upstream touch GPIO47 differs from ChatAI's configured GPIO43.
- Confirm amplifier enable behavior and microphone/audio path physically.
- Do not change GPIO17, GPIO43 or I2S mappings on assumptions.

## 12. Documentation conflict / stale documents

The following repository documents still describe the earlier Gemini-only standalone design and explicitly say no Xiaozhi source is part of the supported firmware:
- `README.md`
- `docs/STANDALONE_MIGRATION.md`

Those statements are **historical/stale relative to source HEAD c251368**. They were not rewritten during this documentation-only history capture, to avoid erasing the earlier design record without a separate review. Future maintainers must treat this history file and `firmware/standalone/XIAOZHI_INTEGRATION.md` as the current Xiaozhi architecture references.

Also, `firmware/standalone/XIAOZHI_INTEGRATION.md` contains a validation-status paragraph saying the integration is source-only and has not been built/tested. That paragraph predates run #97 and the user's install report. The new evidence in §9 supersedes that old validation statement as of 2026-11-26; the rest of the integration details remain useful, subject to verification against current source.

## 13. How to resume research

1. Read `docs/ECC_SHIN_PROJECT_CONTINUATION.md`.
2. Read this history file.
3. Read `firmware/standalone/XIAOZHI_INTEGRATION.md` and `docs/HARDWARE.md`.
4. Check current `main` HEAD and working tree before any edit.
5. Inspect exact current source and file SHA before editing.
6. For any voice/audio change, trace ownership across ChatAI AudioManager → codec bridge → Xiaozhi AudioService/runtime → protocol/session manager.
7. For any network change, preserve NetworkManager ownership and inspect activation/reconnect paths.
8. For build failures, inspect the complete latest failed job log and fix the first real root cause, not just the last warning.
9. Ask for explicit authorization before running Actions/build/flash/OTA.
10. Update this history and continuation guide when a new architecture milestone or verified regression/fix occurs.

**Quick retrieval phrases:** “Mở hồ sơ Xiaozhi”, “Mở lịch sử chuyển đổi ChatAI TPIOS sang Xiaozhi”, “Khôi phục bối cảnh tích hợp Xiaozhi”.
