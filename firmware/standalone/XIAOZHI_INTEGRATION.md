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

## Audio codec contract milestone

- Added a board-neutral `AudioCodec` declaration whose class definition tokens match the pinned upstream v2.6.0 declaration; only the upstream `board.h` include and surrounding board-specific macros are excluded.
- Implemented ChatAI-owned base methods in `main/audio_codec.cpp`; persistent settings remain owned by ChatAI.
- Added `ChatAIXiaozhiAudioCodec`, which adapts the contract to `ChatAIAudioCodecBridge` and therefore to ChatAI's existing `AudioManager` I2S channels.
- Input/output remain mono PCM16 at 16 kHz / 24 kHz respectively, matching the current ChatAI audio manager.
- Added Espressif `esp_audio_codec ~2.5.0` and a ChatAI-owned `ChatAIOpusCodec` adapter. The encoder accepts one mono 16 kHz / 60 ms PCM frame (960 samples); the decoder follows the server's negotiated sample rate and frame duration.
- The standard Xiaozhi server hello advertises 24 kHz mono / 60 ms downlink, which matches the current 24 kHz speaker path. If a server negotiates another supported rate, the runtime must resample before writing to the speaker.
- The codec adapter is not yet instantiated by `app_main`; no audio capture/playback tasks or network transport are active yet. Do not claim end-to-end Xiaozhi voice operation.
- Audio Runtime task lifecycle and server-selected MQTT/UDP or WebSocket transport integration remain subsequent steps.

The board-neutral header must remain definition-compatible with the pinned upstream declaration. Do not compile upstream `audio_codec.cc`, which reads Xiaozhi `Settings` and is not part of ChatAI's settings ownership.

## Hardware integration constraints

- Keep ChatAI's current board pin configuration as the source of truth until verified against the physical XH-S3E-AI V1.0 schematic/board profile.
- Do not instantiate a second I2S driver alongside ChatAI's existing I2S channels.
- Do not copy the `bread-compact-wifi` board configuration from the supplied Xiaozhi firmware image into ChatAI.
- Keep the existing NVS data, Wi-Fi credentials, Web Admin, partition layout, PSRAM settings, and OTA behavior.
- Do not erase or repartition flash as part of this integration.

## Validation status

This milestone contains source changes only. It has not been built with ESP-IDF or tested on hardware. Do not run Codemagic or GitHub Actions; the user runs the GitHub Actions workflow manually.
