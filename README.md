# ChatAI TPIOS — ESP32-S3

Firmware project based on the upstream XiaoZhi ESP32 codebase, pinned to **v2.6.0** for reproducible integration.

## Current repository state

This repository contains the ChatAI TPIOS hardware-board overlay, Gemini Live protocol, PCM bridge integration, local Web Admin, and an idempotent source patcher. The pinned upstream firmware is fetched at preparation time; it is not copied wholesale into this repository. OTA is deferred and is not part of the current basic firmware release.

- Target: ESP32-S3
- Board profile: ChatAI TPIOS (separate board identity; does not overwrite the upstream Bread Compact profile)
- Display profile: SSD1306, 128×64, I²C address 0x3C
- Audio: speaker I²S pins 15/16/7 and amplifier enable GPIO17 are source-reported; microphone GPIOs remain unverified
- Upstream source: https://github.com/78/xiaozhi-esp32
- Pinned upstream tag: `v2.6.0`
- Pinned upstream commit: `4de00aed740595a8713b01f8332b9b2cf30685bc`

## Last successful build

The manual GitHub Actions workflow completed successfully on 2026-11-17:

- Run: [Build ChatAI TPIOS firmware #3](https://github.com/Truongpink/esp32-claude/actions/runs/44046363363)
- Branch / commit: `main` / `39d50a15c64447eb33ba0733af0ee5b57ab512de`
- Toolchain: ESP-IDF v6.1, target `esp32s3`
- All workflow steps passed, including upstream preparation, compilation and artifact upload.
- The current workflow has since been changed to produce `ChatAI-TPIOS-v0.1.1-USB-FLASH` from `merged-binary.bin`; that newer workflow configuration has not yet been run.

This is a successful CI compile, not yet a physical-board validation. The image has not been confirmed flashed or tested on the user's device.

## Implemented source features

- Separate ChatAI TPIOS ESP32-S3 board overlay and OLED SSD1306 128×64 configuration.
- Reproducible fetch of the pinned XiaoZhi source and idempotent overlay patching.
- Initial Gemini Live WebSocket protocol and PCM bridge (16 kHz mono PCM16 input, 24 kHz mono PCM16 output), with transcription, turn-complete and interruption handling.
- Gemini Live is selected directly as the device protocol; the current firmware does not fall back to the XiaoZhi cloud protocol.
- Local Web Admin for status and Gemini settings, without PIN/login as requested.
- OTA upload and update flow are deferred; the current Web Admin exposes device status and Gemini configuration only.
- Manual-only GitHub Actions build workflow; no push/PR build triggers.

## Next phase: physical bring-up

1. Download `xiaozhi.bin` and flash over USB/serial with the matching partition table.
2. Verify boot, OLED, Wi-Fi and local Web Admin on the actual board.
3. Verify microphone wiring/pins; GPIO 4/5/6 remains provisional due conflicting upstream reports.
4. Test speaker output at 24 kHz, microphone capture, two-way Gemini Live voice, buttons and volume.
5. Test OTA only after confirming the installed partition table and first known-good boot.
6. Then improve reconnect/session resumption and production credential provisioning.

The exact hardware revision and microphone mapping are not yet confirmed. The local Web Admin uses plain HTTP on a trusted LAN and the current direct API-key/NVS path is bring-up scope, not production-grade security. Do not expose port 80 to the public Internet.

## Prepare and build locally

Install ESP-IDF **6.0.1 or newer** and its ESP32-S3 toolchain first. From the repository root run:

```bash
python3 scripts/prepare_upstream.py
cd xiaozhi-src
idf.py set-target esp32s3
idf.py build
```

The preparation script fetches the pinned upstream commit, verifies the exact revision, and applies the board/protocol overlay. It refuses to replace a non-Git directory or silently switch an existing checkout to another commit. It does not flash hardware or start any GitHub Actions workflow.

## Documentation

- [Project checkpoint](docs/CHECKPOINT.md)
- [Gemini Live audio integration design](docs/GEMINI_AUDIO_DESIGN.md)
- [Gemini Live wire protocol](docs/GEMINI_LIVE_WIRE_PROTOCOL.md)
- [Hardware evidence and unresolved pins](docs/HARDWARE.md)
- [Local Web Admin](docs/WEB_ADMIN.md)
- [OTA upload design (deferred; not included in the current firmware)](docs/OTA_WEB_UPLOAD.md)

The workflow is manual-only: Actions → Build ChatAI TPIOS firmware → Run workflow. Do not add automatic push/PR triggers.
