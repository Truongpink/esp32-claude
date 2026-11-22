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

## Audio codec and runtime milestones

- Added a board-neutral `AudioCodec` declaration whose class definition tokens match the pinned upstream v2.6.0 declaration; only the upstream `board.h` include and surrounding board-specific macros are excluded.
- Implemented ChatAI-owned base methods in `main/audio_codec.cpp`; persistent settings remain owned by ChatAI.
- Added `ChatAIXiaozhiAudioCodec`, which adapts the contract to `ChatAIAudioCodecBridge` and therefore to ChatAI's existing `AudioManager` I2S channels.
- Input/output remain mono PCM16 at 16 kHz / 24 kHz respectively, matching the current ChatAI audio manager.
- Added Espressif `esp_audio_codec ~2.5.0` and a ChatAI-owned `ChatAIOpusCodec` adapter. The encoder accepts one mono 16 kHz / 60 ms PCM frame (960 samples); the decoder follows the negotiated sample rate and frame duration.
- Added `XiaozhiAudioRuntime` with separate FreeRTOS microphone-capture and speaker-playback tasks. It reuses the existing AudioManager I2S channels; it does not create another I2S driver.
- The runtime exposes a bounded uplink Opus queue for the future network task and accepts complete downlink Opus packets from that task. Both queues are limited to four packets; the oldest packet is dropped when full to bound real-time latency.
- The current runtime accepts 24 kHz mono / 60 ms downlink only, matching the standard Xiaozhi server hello and ChatAI speaker rate. Other negotiated formats are rejected until a resampler is integrated; they are not played at the wrong speed.
- `app_main` now initializes the Xiaozhi session manager after the existing Gemini client; the manager waits for Wi-Fi and performs OTA discovery in its own worker task.
- Runtime lifecycle waits for both worker tasks to exit before stopping the codec or disabling I2S. Keep the runtime object alive for the full duration of any active session.
- Added `ChatAI Xiaozhi OTA config discovery` against the pinned upstream default endpoint `https://api.tenclass.net/xiaozhi/ota/`. It sends a version-2 device report over HTTPS with certificate-bundle verification, a hardware MAC, and a stable UUID v4 persisted in the dedicated `chatai_xiaozhi` NVS namespace.
- The OTA report includes the running app/chip/partition metadata and explicitly advertises MCP, AEC, UDP probe and glyph push as unavailable; it does not claim unsupported features.
- The response parser follows upstream selection precedence: MQTT configuration takes priority when present; otherwise WebSocket is selected. Credentials are held in the returned in-memory config and are never written to logs.
- If the service requires activation, the client retains and displays the activation message/code, then polls `POST /xiaozhi/ota/activate` with Activation-Version 1 and an empty JSON body. HTTP 202 waits three seconds; HTTP 200 completes binding and triggers a fresh OTA query. The session waits for activation before opening audio.
- Added a native ESP-MQTT control adapter with TLS certificate-bundle verification, OTA-provided broker credentials, publish topic, and P2P response-topic subscription. A full `subscribe_topic` such as `devices/p2p/<mac>` is preserved; legacy root-only values are expanded to `<root>/p2p/<client_id>`. If the OTA response omits the field, the current Xiaozhi gateway default `devices/p2p/<mac>` is used.
- Added the MQTT hello exchange and UDP audio session manager. It validates the negotiated server format (24 kHz mono / 60 ms), opens a connected UDP socket, imports the AES-128 key into PSA Crypto, encrypts/decrypts the 16-byte Xiaozhi audio header and Opus payload with AES-CTR, and enforces monotonically increasing receive sequence numbers.
- The session manager starts the existing Xiaozhi audio runtime only after a valid hello response. Microphone capture remains disabled until a listen-start command is sent; BOOT toggles continuous listening and the existing PTT touch input controls manual listening while a Xiaozhi session is ready.
- When Xiaozhi takes the audio session, Gemini continuous listening and PTT are disabled and any active Gemini playback is aborted. Gemini source/settings and the Web Admin baseline remain intact.
- The manager honors OTA transport precedence: MQTT+UDP is used when supplied; otherwise the WebSocket adapter uses the OTA URL/token and Device-Id/Client-Id/Protocol-Version handshake headers. It supports raw Opus WebSocket frames (version 1) and the version 2/3 binary headers. Both transports share the same bounded audio runtime and listen-state controls.

The board-neutral header must remain definition-compatible with the pinned upstream declaration. Do not compile upstream `audio_codec.cc`, which reads Xiaozhi `Settings` and is not part of ChatAI's settings ownership.

## Hardware integration constraints

- Keep ChatAI's current board pin configuration as the source of truth until verified against the physical XH-S3E-AI V1.0 schematic/board profile.
- Do not instantiate a second I2S driver alongside ChatAI's existing I2S channels.
- Do not copy the `bread-compact-wifi` board configuration from the supplied Xiaozhi firmware image into ChatAI.
- Keep the existing NVS data, Wi-Fi credentials, Web Admin, partition layout, PSRAM settings, and OTA behavior.
- Do not erase or repartition flash as part of this integration.

## Validation status

The MQTT/UDP session integration is source-only at this stage. It has not been built with ESP-IDF or tested on hardware. Do not run Codemagic or GitHub Actions; the user runs the GitHub Actions workflow manually.
