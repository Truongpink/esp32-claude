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
- `main/gemini_live_client.*` and Gemini settings remain in the tree for a later phase; Gemini is not initialized, compiled, or used by the Xiaozhi voice path.
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
- `app_main` initializes the Xiaozhi session manager after ChatAI Wi-Fi, display and I2S setup; Gemini Live remains disabled. The manager waits for Wi-Fi and performs OTA discovery in its own worker task.
- Runtime lifecycle waits for both worker tasks to exit before stopping the codec or disabling I2S. Keep the runtime object alive for the full duration of any active session.
- Added `ChatAI Xiaozhi OTA config discovery` against the pinned upstream default endpoint `https://api.tenclass.net/xiaozhi/ota/`. It sends a version-2 device report over HTTPS with certificate-bundle verification, a hardware MAC, and a stable UUID v4 persisted in the dedicated `chatai_xiaozhi` NVS namespace.
- The OTA report includes the running app/chip/partition metadata and explicitly advertises MCP, AEC, UDP probe and glyph push as unavailable; it does not claim unsupported features.
- The response parser follows upstream selection precedence: MQTT configuration takes priority when present; otherwise WebSocket is selected. Credentials are held in the returned in-memory config and are never written to logs.
- If the service requires activation, the client retains and displays the activation message/code, then polls `POST /xiaozhi/ota/activate` with Activation-Version 1 and an empty JSON body. HTTP 202 waits three seconds; HTTP 200 completes binding and triggers a fresh OTA query. The session waits for activation before opening audio.
- Added a native ESP-MQTT control adapter with TLS certificate-bundle verification, OTA-provided broker credentials, publish topic, and P2P response-topic subscription. A full `subscribe_topic` such as `devices/p2p/<mac>` is preserved; legacy root-only values are expanded to `<root>/p2p/<mac>` using the MAC with colons replaced by underscores. If the field is omitted, the default is `devices/p2p/<mac>`.
- Added the MQTT hello exchange and UDP audio session manager. It validates the negotiated server format (24 kHz mono / 60 ms), opens a connected UDP socket, imports the AES-128 key into PSA Crypto, encrypts/decrypts the 16-byte Xiaozhi audio header and Opus payload with AES-CTR, and enforces monotonically increasing receive sequence numbers.
- Before HTTPS OTA/activation, the client synchronizes system time through ESP-NETIF SNTP when the clock is invalid, then applies the OTA `server_time` value for subsequent TLS sessions. The activation-v1 poll requires a non-empty server challenge but sends the documented empty JSON body and the same Device-Id/Client-Id headers.
- The session manager starts the existing Xiaozhi audio runtime only after a valid hello response. The microphone peripheral stays disabled while idle and is enabled only after a listen-start command; server TTS start/stop pauses and resumes capture. A single BOOT press starts one `auto` listen turn; Xiaozhi server-side VAD determines the end of speech, and the client sends `listen/stop` after TTS playback completes so the device returns to idle. A later turn requires another BOOT press. The existing PTT touch input remains a separate hold-to-talk manual path while a Xiaozhi session is ready.
- Xiaozhi is the only enabled voice client. Its audio runtime owns microphone capture and speaker playback for the duration of a session; no Gemini worker or Gemini audio arbitration is started.
- The manager honors OTA transport precedence: MQTT+UDP is used when supplied; otherwise the WebSocket adapter uses the OTA URL/token and Device-Id/Client-Id/Protocol-Version handshake headers. It supports raw Opus WebSocket frames (version 1) and the version 2/3 binary headers. Both transports share the same bounded audio runtime and listen-state controls.

The board-neutral header must remain definition-compatible with the pinned upstream declaration. Do not compile upstream `audio_codec.cc`, which reads Xiaozhi `Settings` and is not part of ChatAI's settings ownership.

## Hardware integration constraints

- Keep ChatAI's current board pin configuration as the source of truth until verified against the physical XH-S3E-AI V1.0 schematic/board profile.
- Do not instantiate a second I2S driver alongside ChatAI's existing I2S channels.
- Do not copy the `bread-compact-wifi` board configuration from the supplied Xiaozhi firmware image into ChatAI.
- Keep the existing NVS data, Wi-Fi credentials, Web Admin, partition layout, PSRAM settings, and OTA behavior.
- Do not erase or repartition flash as part of this integration.

## Current AFE feature availability on the ChatAI board

- FD AFE voice processing and VAD are enabled. With no VAD model supplied, ESP-SR uses its WebRTC VAD fallback. ChatAI configures VAD mode 2, a 64 ms minimum speech window, a 64 ms VAD delay, and WebRTC AGC (12 dB compression gain, -3 dBFS target). `MIC_VAD` records speech/silence transitions; `MIC_LEVEL` reports pre-AFE PCM RMS/peak levels every two seconds while capture is active. The fixed 120 ms delay before the first microphone read has been removed. The codec bridge does not expose hardware microphone gain, so do not claim a hardware preamp adjustment.
- WakeNet is not active: the standalone partition table has no `model` partition and this repository does not ship an ESP-SR model image. `esp_srmodel_init("model")` therefore cannot load WakeNet models. Wake-word audio upload is also disabled.
- Device AEC is not active: `ChatAIXiaozhiAudioCodec` exposes one microphone channel and declares no playback-reference channel. The current speaker output callback is not a calibrated, synchronized AFE reference path.
- Noise suppression is disabled because no NSNet model is provisioned. WebRTC AGC is enabled with 12 dB compression gain and a -3 dBFS target; this is dynamic gain control, not a fixed microphone gain. PCM input level is measured by `MIC_LEVEL` diagnostics. The current codec bridge does not support hardware input-gain control.
- Do not report WakeNet, AEC, or NS as operational until the required model assets and/or verified reference signal path are supplied. The existing assets partition and OTA layout must not be repurposed or changed without explicit approval.

## Validation status

The MQTT/UDP and WebSocket session integration is source-only at this stage. It has not been built with ESP-IDF or tested on hardware. Do not run Codemagic or GitHub Actions; the user runs the GitHub Actions workflow manually.
