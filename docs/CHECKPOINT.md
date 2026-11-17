# ChatAI TPIOS — project checkpoint

**Checkpoint date:** 2026-11-17  
**Branch:** `main`  
**Last successful build source revision:** `39d50a15c64447eb33ba0733af0ee5b57ab512de`  
**Build:** GitHub Actions run [#3](https://github.com/Truongpink/esp32-claude/actions/runs/44046363363) — **success**  
**Build environment:** ESP-IDF v6.1, target `esp32s3`  
**Artifact from that run:** `ChatAI-TPIOS-ESP32S3-firmware`, containing `xiaozhi.bin` only. The current workflow now creates `ChatAI-TPIOS-v0.1.1-USB-FLASH` with `ChatAI-TPIOS-v0.1.1-USB-FLASH.bin`; this newer workflow has not yet run.

## Implemented in source

- Added a separate ChatAI TPIOS ESP32-S3 board overlay without replacing upstream board profiles.
- Added a reproducible preparation script that fetches the pinned XiaoZhi v2.6.0 source revision (`4de00aed740595a8713b01f8332b9b2cf30685bc`) and applies the ChatAI overlay.
- Added the Gemini Live protocol implementation and the PCM bridge integration:
  - 16 kHz mono PCM16 microphone uplink.
  - 24 kHz mono PCM16 speaker playback.
  - WebSocket setup handshake, audio streaming, transcription callbacks, turn completion and interruption handling.
  - Gemini Live is selected directly as the device protocol. The current firmware does not fall back to the XiaoZhi cloud protocol when Gemini is disabled or unconfigured.
- Added the board profile and OLED SSD1306 128x64 Kconfig selection.
- Added the local Web Admin source:
  - Device status and Gemini configuration backed by NVS.
  - No PIN/login, as requested; intended for trusted local Wi-Fi only.
  - OTA upload and update flow are deferred; the current Web Admin exposes device status and Gemini configuration only.
- Added a manual-only GitHub Actions workflow. The last successful run uploaded the application-only `xiaozhi.bin`; the current workflow configuration now runs `idf.py merge-bin` and uploads the complete `merged-binary.bin` USB image under the versioned `ChatAI-TPIOS-v0.1.1-USB-FLASH` artifact name. That updated workflow has not yet run.
- Fixed the two compile blockers found in earlier runs: Gemini transcript indentation under `-Werror`, and OLED type/height Kconfig selection.

## What the previous successful build proves

Workflow run #3 completed all steps successfully at its recorded source revision: checkout of `main`, upstream preparation/patch application, ESP-IDF v6.1 compilation, and artifact upload. The artifact is present and has not expired.

This confirms that the earlier source revision and board overlay compiled in the configured CI environment. Later edits on `main`, including the Gemini WebSocket authentication correction, have not yet been rebuilt. It does **not** prove that the image has been flashed or that the physical board's microphone, speaker, OLED, buttons, Gemini network session, Web Admin or OTA flow work on the actual device.

## Remaining work — next phase

1. Run the current manual workflow and download `ChatAI-TPIOS-v0.1.1-USB-FLASH`; flash `ChatAI-TPIOS-v0.1.1-USB-FLASH.bin` over USB while preserving the board's known-good layout.
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
