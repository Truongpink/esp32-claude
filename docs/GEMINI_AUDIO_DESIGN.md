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
- On server interruption, clear queued/in-flight playback promptly before accepting the next audio turn.
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

This document is a source-verified design, not a claim that Gemini Live is already integrated. The next code change should add the raw PCM queue/playback seam first, then implement and test the dedicated Gemini protocol against that seam. Do not create a firmware build workflow or flash image until both paths are integrated and reviewed.


## PCM bridge implementation status

The upstream patcher now adds an optional, bounded PCM bridge to `AudioService`:

- `EnablePcmBridge(true)` routes processed 16 kHz mono PCM frames from `AudioEngine::OnOutput` into a four-frame queue instead of the Opus encoder.
- `PopPcmInput()` lets the Gemini transport drain captured frames without blocking the audio/AFE task.
- `PushPcmPlayback()` queues decoded PCM directly for the existing speaker output task, bypassing Opus decoding.
- `ClearPcmPlayback()` clears queued PCM for interruption handling.
- The bridge defaults to disabled. When disabled, the original Opus encode/decode path remains selected.

This is only the audio-service seam. It does not yet establish a Gemini WebSocket session, base64/JSON framing, 24 kHz output resampling, turn/VAD control, or reconnect/session-resumption behavior. The transport must validate the server's audio sample rate and convert to the codec's output rate before calling `PushPcmPlayback()`. A currently playing output frame may finish when `ClearPcmPlayback()` is called; interruption latency is therefore bounded by the current output frame.

The patch is applied to the pinned upstream source at build preparation time by `scripts/apply_upstream_patches.py`. It has not been compiled or exercised on hardware yet.
