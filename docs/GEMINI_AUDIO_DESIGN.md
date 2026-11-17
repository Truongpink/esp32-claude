# Gemini Live integration — implementation design

## Verified upstream audio path

Pinned source: XiaoZhi ESP32 `v2.6.0`, commit `4de00aed740595a8713b01f8332b9b2cf30685bc`.

The current audio path is not PCM end-to-end:

1. `AudioService::AudioInputTask()` reads microphone frames at 16 kHz and feeds the selected `AudioEngine` (AFE on ESP32-S3).
2. The engine's `OnOutput` callback calls `PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, ...)`.
3. `AudioService::OpusCodecTask()` encodes those frames as Opus before placing them on the send queue.
4. Incoming `AudioStreamPacket` payloads are passed to the Opus decoder and then the playback queue.
5. `Application::InitializeProtocol()` owns protocol selection and wires incoming audio/JSON callbacks.

Relevant source files:
- `main/audio/audio_service.cc`
- `main/audio/audio_service.h`
- `main/audio/audio_engine.h`
- `main/protocols/protocol.h`
- `main/application.cc`

## Required Gemini path

Gemini Live audio must bypass the Opus encoder and decoder:

- Input: mono signed PCM16, 16 kHz, little-endian.
- Output: mono signed PCM16, 24 kHz, little-endian (resample only if the board codec uses another output rate).
- Input packets should be short, bounded chunks (target 20–40 ms); do not accumulate long microphone buffers.
- Outgoing audio is sent as Live API `realtimeInput.audio` JSON content (base64 inline data), not the XiaoZhi binary Opus packet framing.
- Incoming `serverContent.modelTurn.parts[].inlineData` audio is base64-decoded and queued as PCM playback.
- On server interruption or transport failure, disable microphone forwarding and clear queued PCM promptly; the speaker frame already inside the codec may finish.
- Handle setup completion, server content, turn completion, go-away, session resumption and recoverable disconnects as explicit states.

## Minimal integration strategy

1. Add a dedicated `GeminiLiveProtocol` implementing the existing `Protocol` interface. Do not change or remove MQTT/WebSocket XiaoZhi transports.
2. Add a raw-PCM path to `AudioService` behind an explicit runtime mode or narrow API:
   - tap AFE output before the Opus encoder;
   - queue PCM with a bounded queue and drop-oldest behavior under congestion;
   - add a PCM playback enqueue path that bypasses Opus decode;
   - keep the existing Opus queues and codec task unchanged for the original transports.
3. Select Gemini only when ChatAI TPIOS configuration is present. Existing Xiaozhi protocol selection remains the fallback until the Gemini configuration is saved and validated.
4. Keep network callbacks short: parse/copy incoming data, enqueue PCM, and defer UI/state changes through the existing application scheduler.
5. Store credentials in NVS; never return or log the full key. A blank key submitted from Web Admin means keep the stored key. Prefer short-lived Gemini ephemeral credentials from a trusted token broker where available; a long-lived API key on an ESP32 is extractable by a sufficiently capable physical attacker.
6. Keep Gemini model name, API endpoint and protocol schema in one configuration module, not scattered literals.

## Resource and reliability constraints

- Base64 increases payload size; use bounded frame sizes and avoid multiple full-size copies.
- Do not block the AFE/audio callback on network I/O.
- Use a dedicated bounded send queue and a network task for WebSocket writes.
- Track queue drops, reconnect count, setup state and last server error in a small diagnostic status structure; do not log API keys or full audio payloads.
- On disconnect, stop sending stale microphone frames, clear stale playback, reconnect with bounded backoff and re-run setup/session resumption as supported.
- Preserve the existing device button and AFE/VAD behavior unless a dedicated Gemini interaction mode requires a narrow change.

## Not implemented yet

This document records both the source-verified design and the current implementation. The raw PCM queue/playback seam, initial Gemini protocol, local Web Admin/NVS credential form, and streamed `.bin` OTA handler now exist in source. Remaining work includes bounded reconnect/session resumption, short-lived token provisioning, static compile validation, and hardware testing. Do not run a build workflow or flash image without explicit user approval.


## PCM bridge implementation status

The upstream patcher now adds an optional, bounded PCM bridge to `AudioService`:

- `EnablePcmBridge(true)` routes processed 16 kHz mono PCM frames from `AudioEngine::OnOutput` into a four-frame queue instead of the Opus encoder. The pinned upstream initializes the AFE with `OPUS_FRAME_DURATION_MS = 60`, so each frame is 960 samples and the queue holds at most about 240 ms of captured audio.
- `PopPcmInput()` lets the Gemini transport drain captured frames without blocking the audio/AFE task.
- `PushPcmPlayback()` queues decoded PCM directly for the existing speaker output task, bypassing Opus decoding.
- `ClearPcmPlayback()` clears queued PCM for interruption handling.
- The bridge defaults to disabled. When disabled, the original Opus encode/decode path remains selected.

The initial Gemini transport establishes a WebSocket setup session, splits 60 ms capture frames into three 20 ms uplink chunks, validates 24 kHz output MIME data, decodes PCM and queues it directly for `AudioOutputTask`. Any fatal protocol/transport error now disables PCM capture, clears queued speaker PCM and exits the PCM playback mode before notifying the application. The target board codec is configured for 24 kHz, so no resampler is required for this board profile. PCM playback has a separate eight-task capacity while the original Opus decode path retains its two-task limit. `ClearPcmPlayback()` clears queued output and signals the existing playback-drained callback when appropriate; a currently playing frame may finish. Reconnect/session resumption remain outstanding. See [Gemini Live wire protocol](GEMINI_LIVE_WIRE_PROTOCOL.md) for the setup gate, JSON frames, PCM formats and control events.

The patch is applied to the pinned upstream source at build preparation time by `scripts/apply_upstream_patches.py`. It has not been compiled or exercised on hardware yet.


## Application event-loop integration findings

The existing `Application` main loop still drains Opus packets on `MAIN_EVENT_SEND_AUDIO`. The patcher also maps raw-PCM-ready notifications to `MAIN_EVENT_PROTOCOL_PENDING`; the main loop invokes `GeminiLiveProtocol::ProcessPending()` there. The receive callback parses bounded JSON/audio payloads and queues PCM with `PushPcmPlayback()`; speaker output remains in `AudioOutputTask`.

**Turn-by-turn source trace (pinned upstream plus the ChatAI patcher):**

1. The BOOT/chat toggle in Idle opens the Gemini WebSocket, waits for `setupComplete`, then enters `kDeviceStateListening`.
2. `StartListeningAudio()` calls `GeminiLiveProtocol::SendStartListening()` (enables the PCM bridge) and then enables AFE voice processing.
3. AFE `OnOutput` sends processed PCM frames to the bounded PCM queue. The callback raises `MAIN_EVENT_PROTOCOL_PENDING`; `ProcessPending()` sends 16 kHz PCM to `realtimeInput.audio`. Gemini's server VAD determines user-turn boundaries; the firmware does not synthesize a text request and call a separate TTS service.
4. Gemini response audio arrives as 24 kHz PCM, is queued directly to the speaker, and is bracketed by the protocol's legacy `type: "tts"` UI events. In `Application::OnIncomingJson`, those events only change the Speaking/Listening display state and render transcript text; they do not invoke a TTS API.
5. The selected listening mode is forced to `kListeningModeRealtime`. Consequently, the application does not disable AFE voice processing while Speaking. `EmitTtsStop()` returns the UI to Listening without disabling the PCM bridge; microphone forwarding remains active between assistant turns. Explicit stop/close and fatal transport errors still disable it.

This establishes that the source is designed for Gemini Live voice-to-voice streaming and continuous multi-turn capture. It does **not** prove that the physical microphone is reaching the server continuously, that speaker echo is adequately cancelled, or that barge-in is reliable on the user's board; those require a firmware build and hardware test. The `tts` label is legacy UI event naming and is potentially confusing, but changing it would require a coordinated application/protocol event contract change and is not necessary for audio transport.

The upstream `WebSocket` implementation is supplied by `78/esp-ml307` and is exposed through `Board::GetNetwork()->CreateWebSocket()`. Its send API accepts text frames and its default maximum outgoing payload is 65,535 bytes. The current transport code authenticates using an API-key query parameter; never log the full URL. Production should move to short-lived credentials from a trusted token broker.


## Gemini Live transport implementation status

A dedicated `GeminiLiveProtocol` source is now included by the upstream patcher. It reuses the existing board WebSocket transport, waits for `setupComplete`, streams 16 kHz AFE frames as three 20 ms PCM JSON chunks, decodes 24 kHz PCM output, and routes interruption/transcription/turn-complete events through existing application callbacks. This board declares 24 kHz speaker output, matching Gemini Live output, so no output resampler is required. Gemini is selected only when NVS namespace `gemini` has `enabled=true` and a non-empty `api_key`; optional `system_prompt` is limited to an NVS-compatible key name; otherwise the existing XiaoZhi protocol selection remains active.

Direct-device API-key mode is for bring-up only: use a current Gemini authorization API key, store it in NVS, and send it in the `x-goog-api-key` handshake header. This avoids URL-path logs, but a sufficiently capable physical attacker can still extract the key. Production should use short-lived tokens from a trusted broker. The local Web Admin credential form and OTA handler are implemented in source, but neither this transport nor the Web Admin/OTA path has been compiled or exercised on hardware.
