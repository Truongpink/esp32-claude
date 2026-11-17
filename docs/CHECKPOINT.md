# ChatAI TPIOS — project checkpoint

**Checkpoint date:** 2026-11-17  
**Branch:** `main`  
**Last successful build source revision:** `39d50a15c64447eb33ba0733af0ee5b57ab512de`  
**Build:** GitHub Actions run [#3](https://github.com/Truongpink/esp32-claude/actions/runs/44046363363) — **success**  
**Build environment:** ESP-IDF v6.1, target `esp32s3`  
**Artifact from that run:** `ChatAI-TPIOS-ESP32S3-firmware`, containing `xiaozhi.bin` only. The current workflow renames the generated USB image to `ChatAI-TPIOS-v0.1.1-USB-FLASH.bin` and uploads it under `ChatAI-TPIOS-v0.1.1-USB-FLASH`; this newer workflow has not yet run.

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
  - Device status, Gemini configuration, and speaker volume backed by NVS.
  - Original dark/purple AI Voice Bot layout adapted for the actual supported device controls.
  - Streamed application-only OTA upload to the inactive app partition, with runtime dual-slot checks and ESP-IDF image validation.
  - No PIN/login; intended for trusted local Wi-Fi only.
- The manual-only GitHub Actions workflow now publishes two separate artifacts: application-only `ChatAI-TPIOS-v0.1.1-OTA.bin` for Web Admin OTA and merged `ChatAI-TPIOS-v0.1.1-USB-FLASH.bin` for USB flashing. This updated workflow has not yet run.
- The OTA patch marks a pending image valid after application/protocol initialization completes without a startup error, allowing the ESP-IDF bootloader rollback mechanism to restore the previous slot if the new image fails before validation.
- Fixed the two compile blockers found in earlier runs: Gemini transcript indentation under `-Werror`, and OLED type/height Kconfig selection.

## What the previous successful build proves

Workflow run #3 completed all steps successfully at its recorded source revision: checkout of `main`, upstream preparation/patch application, ESP-IDF v6.1 compilation, and artifact upload. The artifact is present and has not expired.

This confirms that the earlier source revision and board overlay compiled in the configured CI environment. Later edits on `main`, including the Gemini WebSocket authentication correction, have not yet been rebuilt. It does **not** prove that the image has been flashed or that the physical board's microphone, speaker, OLED, buttons, Gemini network session, Web Admin or OTA flow work on the actual device.

## Remaining work — next phase

1. Run the current manual workflow and download `ChatAI-TPIOS-v0.1.1-USB-FLASH`; flash `ChatAI-TPIOS-v0.1.1-USB-FLASH.bin` over USB while preserving the board's known-good layout.
2. Bring up the device and verify boot, OLED logo/status, Wi-Fi connection and local Web Admin.
3. Verify the exact board revision and microphone pin mapping on the physical PCB. GPIO 4/5/6 is still provisional because upstream reports conflict.
4. Test speaker output at the configured 24 kHz rate, microphone capture, two-way Gemini Live conversation, touch and volume controls.
5. Run the updated manual workflow, verify both the application-only OTA image and merged USB image are produced, then test Web Admin OTA and rollback on the actual board.
6. Verify the new Gemini WebSocket frame diagnostics on hardware, then improve reconnect/session resumption and review transport shutdown/backpressure.
7. Before production use, replace long-lived device API-key provisioning with a safer short-lived token flow; current local HTTP/NVS setup is bring-up scope and is not production security.

## Working rules

- Work on `main` only; do not create or switch branches.
- Inspect current repository state and exact source before every edit.
- Keep changes minimal and preserve upstream XiaoZhi functionality.
- GitHub Actions is manual-only. Do not dispatch builds or flash hardware without the user's explicit instruction.
- Do not call the firmware hardware-validated until the physical bring-up tests above have been completed.
