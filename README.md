# ChatAI TPIOS — ESP32-S3

Firmware project based on the upstream XiaoZhi ESP32 codebase, pinned to **v2.6.0** for reproducible integration.

## Current repository state

This repository contains the ChatAI TPIOS hardware-board overlay, Gemini Live protocol, local Web Admin, OTA upload handler, and an idempotent source patcher. The pinned upstream firmware is fetched at preparation time; it is not copied wholesale into this repository.

- Target: ESP32-S3
- Board profile: ChatAI TPIOS (separate board identity; does not overwrite the upstream Bread Compact profile)
- Display profile: SSD1306, 128×64, I²C address 0x3C
- Audio: speaker I²S pins 15/16/7 and amplifier enable GPIO17 are source-reported; microphone GPIOs remain unverified
- Upstream source: https://github.com/78/xiaozhi-esp32
- Pinned upstream tag: `v2.6.0`
- Pinned upstream commit: `4de00aed740595a8713b01f8332b9b2cf30685bc`

## Prepare and build locally

Install ESP-IDF **6.0.1 or newer** and its ESP32-S3 toolchain first. From the repository root run:

```bash
python3 scripts/prepare_upstream.py
cd xiaozhi-src
idf.py set-target esp32s3
idf.py build
```

The preparation script fetches the pinned upstream commit, verifies the exact revision, and applies the board/protocol overlay. It refuses to replace a non-Git directory or silently switch an existing checkout to another commit. It does not flash hardware or start any GitHub Actions workflow.

## Architecture notes

See [Gemini Live audio integration design](docs/GEMINI_AUDIO_DESIGN.md) and [Gemini Live wire protocol](docs/GEMINI_LIVE_WIRE_PROTOCOL.md) for the protocol and PCM pipeline. See [hardware evidence and unresolved pins](docs/HARDWARE.md) before selecting GPIOs, [local Web Admin](docs/WEB_ADMIN.md) for settings, and [OTA upload](docs/OTA_WEB_UPLOAD.md) for the local `.bin` update path.

## Important status

The board overlay, initial Gemini Live PCM/WebSocket transport, and a basic unauthenticated local Web Admin are present. The Web Admin provides device status, Gemini configuration backed by NVS, and streamed .bin OTA to the inactive application slot. OTA selects the slot from the device's active partition table and validates the image before switching boot partitions. The Gemini path, Web Admin and OTA handler have not been compile-tested or hardware-validated. Bounded reconnect/session resumption and secure short-lived token provisioning remain outstanding. Do not treat this as finished or production-ready firmware.

A manual-only GitHub Actions workflow is available at Actions → Build ChatAI TPIOS firmware → Run workflow. It prepares the pinned upstream source, builds for ESP32-S3 with ESP-IDF v6.1, and uploads only `xiaozhi.bin` as the application firmware artifact. It does not run on push or pull requests. The workflow has been added but has not yet been dispatched or build-validated.
