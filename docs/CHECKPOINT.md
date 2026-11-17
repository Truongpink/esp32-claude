# ChatAI TPIOS — project checkpoint

**Checkpoint date:** 2026-11-17  
**Branch:** `main`  
**Source revision:** `39d50a15c64447eb33ba0733af0ee5b57ab512de`  
**Build:** GitHub Actions run [#3](https://github.com/Truongpink/esp32-claude/actions/runs/44046363363) — **success**  
**Build environment:** ESP-IDF v6.1, target `esp32s3`  
**Artifact:** `ChatAI-TPIOS-ESP32S3-firmware`, containing the application image `xiaozhi.bin` only.

## Completed and build-verified

- Added a separate ChatAI TPIOS ESP32-S3 board overlay without replacing upstream board profiles.
- Added a reproducible preparation script that fetches the pinned XiaoZhi v2.6.0 source revision (`4de00aed740595a8713b01f8332b9b2cf30685bc`) and applies the ChatAI overlay.
- Added the Gemini Live protocol implementation and the PCM bridge integration:
  - 16 kHz mono PCM16 microphone uplink.
  - 24 kHz mono PCM16 speaker playback.
  - WebSocket setup handshake, audio streaming, transcription callbacks, turn completion and interruption handling.
  - Gemini selection through NVS settings; existing XiaoZhi protocol remains the fallback when Gemini is not enabled/configured.
- Added the board profile and OLED SSD1306 128x64 Kconfig selection.
- Added the local Web Admin source:
  - Device status and Gemini configuration backed by NVS.
  - No PIN/login, as requested; intended for trusted local Wi-Fi only.
  - Streamed application `.bin` OTA endpoint targeting the inactive app partition and validating the image before selecting it.
- Added a manual-only GitHub Actions workflow. It prepares upstream source, builds for ESP32-S3 and uploads only `xiaozhi.bin` as the workflow artifact payload.
- Fixed the two compile blockers found in earlier runs: Gemini transcript indentation under `-Werror`, and OLED type/height Kconfig selection.

## What the successful build proves

The latest workflow completed all steps successfully: checkout of `main`, upstream preparation/patch application, ESP-IDF v6.1 compilation, and artifact upload. The artifact is present and has not expired.

This confirms that the firmware source and board overlay compile in the configured CI environment. It does **not** prove that the image has been flashed or that the physical board's microphone, speaker, OLED, buttons, Gemini network session, Web Admin or OTA flow work on the actual device.

## Remaining work — next phase

1. Download `xiaozhi.bin` and perform the first USB/serial flash using the matching partition table; preserve the board's known-good layout.
2. Bring up the device and verify boot, OLED logo/status, Wi-Fi connection and local Web Admin.
3. Verify the exact board revision and microphone pin mapping on the physical PCB. GPIO 4/5/6 is still provisional because upstream reports conflict.
4. Test speaker output at the configured 24 kHz rate, microphone capture, two-way Gemini Live conversation, touch and volume controls.
5. Test Web Admin Gemini settings and OTA only after the first installation and actual OTA partition layout are confirmed.
6. Improve Gemini Live reconnect/session resumption and review transport shutdown/backpressure after basic hardware operation is proven.
7. Before production use, replace long-lived device API-key provisioning with a safer short-lived token flow; current local HTTP/NVS setup is bring-up scope and is not production security.

## Working rules

- Work on `main` only; do not create or switch branches.
- Inspect current repository state and exact source before every edit.
- Keep changes minimal and preserve upstream XiaoZhi functionality.
- GitHub Actions is manual-only. Do not dispatch builds or flash hardware without the user's explicit instruction.
- Do not call the firmware hardware-validated until the physical bring-up tests above have been completed.
