# Xiaozhi voice integration baseline

## Pinned upstream source

- Upstream: https://github.com/78/xiaozhi-esp32
- Release tag: `v2.6.0`
- Pinned commit: `4de00aed740595a8713b01f8332b9b2cf30685bc`
- Integration source path: `third_party/xiaozhi-esp32` (Git submodule)
- Upstream license: MIT. Keep the upstream license and copyright notices with any copied or modified upstream source.

The submodule is intentionally pinned to the release commit rather than tracking a moving branch. A checkout that needs this source must initialize/update Git submodules recursively.

## Current ChatAI boundaries

The existing ChatAI standalone firmware remains the application and hardware owner:

- `main/audio_manager.*` owns the current I2S microphone/speaker channels and amplifier control.
- `main/network_manager.*` owns Wi-Fi, provisioning, and the HTTP server lifecycle.
- `main/web_admin.*` owns the existing Web Admin routes and embedded page.
- `main/display_manager.*`, `main/button_manager.*`, and `main/diagnostic_manager.*` remain in place.
- `main/gemini_live_client.*` and Gemini settings remain in the tree for a later phase; this phase does not remove or rewrite them.
- The existing 16 MB partition table and PSRAM defaults are unchanged.

## Integration findings from upstream v2.6.0

The upstream audio pipeline is not a standalone drop-in file. Its AudioService uses an AudioCodec abstraction, Opus encode/decode, resampling, FreeRTOS queues/tasks, and an audio engine. On ESP32-S3, the upstream source selects AFE. AFE depends on ESP-SR model APIs and model assets. The upstream protocol layer supports MQTT/UDP and WebSocket, selected from server-provided configuration; MQTT/UDP is not interchangeable with a WebSocket-only client.

The upstream application also assumes its own Board, NetworkInterface, Settings, Assets, and application lifecycle abstractions. Those must be adapted deliberately rather than importing the upstream board/application startup wholesale.

## Audio adapter milestone

- Added a board-neutral `AudioCodec` interface matching the Xiaozhi v2.6.0 codec contract, without importing Xiaozhi `Board` or `Settings`.
- Added `ChatAIXiaozhiAudioCodec`, which adapts that contract to the existing `ChatAIAudioCodecBridge` and therefore to ChatAI's existing `AudioManager` I2S channels.
- Input/output remain mono PCM16 at 16 kHz / 24 kHz respectively, matching the current ChatAI audio manager.
- The adapter is compiled as part of the standalone firmware, but is not yet instantiated by `app_main` and does not yet activate `AudioService`.
- Opus/audio-engine dependencies, AudioService task lifecycle, and network transport integration remain subsequent steps. Do not claim end-to-end Xiaozhi voice operation yet.

## Hardware integration constraints

- Keep ChatAI's current board pin configuration as the source of truth until verified against the physical XH-S3E-AI V1.0 schematic/board profile.
- Do not instantiate a second I2S driver alongside ChatAI's existing I2S channels.
- Do not copy the `bread-compact-wifi` board configuration from the supplied Xiaozhi firmware image into ChatAI.
- Keep the existing NVS data, Wi-Fi credentials, Web Admin, partition layout, PSRAM settings, and OTA behavior.
- Do not erase or repartition flash as part of this integration.

## Validation status

This milestone has source changes only. It has not yet been built with ESP-IDF or tested on hardware. No Codemagic build, GitHub Actions, flash, or OTA has been run.
