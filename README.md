# ChatAI TPIOS — ESP32-S3

Firmware project based on the upstream XiaoZhi ESP32 codebase, pinned to **v2.6.0** for reproducible integration.

## Current repository state

This repository contains the ChatAI TPIOS hardware-board overlay and an idempotent source patcher. The upstream firmware is fetched at build time; it is not copied wholesale into this repository.

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

See [Gemini Live audio integration design](docs/GEMINI_AUDIO_DESIGN.md) for the source-verified audio pipeline and the required PCM integration seam. See [hardware evidence and unresolved pins](docs/HARDWARE.md) before selecting GPIOs, and [Web Admin firmware upload OTA design](docs/OTA_WEB_UPLOAD.md) for the local .bin update path.

## Important status

The board overlay, initial Gemini Live PCM/WebSocket transport, and a basic unauthenticated local Web Admin are present. The Web Admin provides device status and Gemini configuration backed by NVS; firmware .bin upload is not implemented yet. The Gemini path and Web Admin have not been compile-tested or hardware-validated. Bounded reconnect/session resumption and secure short-lived token provisioning remain outstanding. Do not treat this as finished or production-ready firmware.

Build workflows will be added only after the required ChatAI features are integrated and statically checked. GitHub Actions are manual-only and are not run automatically by repository changes.
