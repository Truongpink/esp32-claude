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

## Architecture notes

See [Gemini Live audio integration design](docs/GEMINI_AUDIO_DESIGN.md) for the source-verified audio pipeline and the required PCM integration seam. See [hardware evidence and unresolved pins](docs/HARDWARE.md) before selecting GPIOs, and [Web Admin firmware upload OTA design](docs/OTA_WEB_UPLOAD.md) for the local .bin update path.

## Important status

The board overlay is the hardware bring-up foundation only. Gemini Live transport/audio-session integration and the ChatAI TPIOS Web Admin are **not yet implemented**. Do not treat a baseline XiaoZhi-derived build as the finished ChatAI TPIOS firmware and do not flash it as the final product.

Build workflows will be added only after the required ChatAI features are integrated and statically checked. GitHub Actions are manual-only and are not run automatically by repository changes.
