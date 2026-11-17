# Gemini Live wire protocol for ChatAI TPIOS

This document records the Gemini Live wire contract and distinguishes implemented firmware behavior from remaining work.

## Connection and setup

- Use a TLS WebSocket (WSS) connection to the Gemini Live API endpoint for the selected supported model.
- Authentication options for raw WebSocket clients:
  - Initial bring-up implementation: send the API key in the `x-goog-api-key` WebSocket handshake header. This avoids putting the key in the URL path, which the upstream transport logs. The key is still extractable from device NVS and is not production-grade secret storage.
  - Ephemeral token (preferred for a deployed device): use the `BidiGenerateContentConstrained` endpoint and send the short-lived token in `Authorization: Token <token>` (or `access_token` query parameter). A token broker must authenticate the local device/user and mint the token using the Gemini AuthToken API. Do not put the long-lived API key in firmware or Web Admin.
- Send exactly one `setup` message after the WebSocket is established.
- Do not send audio until the server returns `setupComplete`.
- Keep the selected model name and endpoint in one configuration location. As checked against Google's current Live API documentation on 2026-11-17, `gemini-3.8-live` is a supported Live model and is the firmware default. The model remains configurable through Web Admin/NVS.

The firmware uses the `x-goog-api-key` handshake header. Google's current GenAI Go SDK also sends this header on its Live WebSocket handshake. The Google raw-WebSocket quickstart additionally documents `?key=`; the header avoids placing the long-lived key in the URL path, which this board's WebSocket transport may log.

## Setup message shape

The setup message should request audio output and provide the device's system instruction. A representative JSON shape is:

```json
{
  "setup": {
    "model": "models/<supported-live-model>",
    "generationConfig": {
      "responseModalities": ["AUDIO"]
    },
    "systemInstruction": {
      "parts": [{ "text": "<configured system prompt>" }]
    },
    "realtimeInputConfig": {
      "automaticActivityDetection": {
        "disabled": false
      }
    }
  }
}
```

This is a schema illustration, not a hardcoded model or a complete authentication URL.

## Microphone input

The AFE output bridge supplies signed PCM16 mono at 16 kHz. Serialize each bounded frame as:

```json
{
  "realtimeInput": {
    "audio": {
      "mimeType": "audio/pcm;rate=16000",
      "data": "<base64 little-endian PCM16>"
    }
  }
}
```

Use server-side automatic VAD initially. Send `audioStreamEnd: true` only when the microphone stream is intentionally stopped/closed, not after every short audio frame.

## Speaker output

For each received message:

1. Parse all `serverContent.modelTurn.parts[]` entries; a message can contain more than one part.
2. For every `inlineData` audio part, verify its MIME type indicates PCM at 24 kHz.
3. Base64-decode to signed PCM16 little-endian.
4. Pass the samples to `AudioService::PushPcmPlayback()`. The existing AudioOutputTask writes these samples directly to the codec and bypasses Opus decoding.
5. Do not block the WebSocket receive callback on speaker output.

The current ChatAI TPIOS board profile configures codec output for 24 kHz. The transport accepts only 24 kHz PCM output; other MIME rates are rejected rather than played at the wrong speed. A future board profile with a different output rate must add resampling.

## Control events

- `setupComplete`: connection is ready for realtime input.
- `serverContent.interrupted == true`: call `ClearPcmPlayback()` immediately and discard any not-yet-queued audio from the interrupted turn.
- `serverContent.turnComplete == true`: mark the logical turn complete; do not assume the hardware playback queue has already drained.
- `goAway`: stop sending new audio and reconnect/resume according to the current API contract.
- `sessionResumptionUpdate`: persist the opaque resumption handle only for the active device session and use it according to the API's expiry rules.
- WebSocket close/error: stop mic forwarding and clear stale speaker audio. Fatal errors currently return the application to idle; bounded exponential reconnect and session resumption are still pending.

## Memory and task rules

- The receive callback must only parse/copy bounded payloads and enqueue work; it must not call blocking codec APIs.
- Never buffer an entire conversation or an unbounded audio stream in RAM.
- Base64 temporarily increases payload size; size all message buffers for the encoded form and reject oversized frames. The current WebSocket wrapper rejects outgoing payloads over 65,535 bytes and logs the URL path at debug level; never put credentials in logged URL text.
- Do not log API keys, bearer tokens, full setup URLs containing credentials, or audio payloads.
- Keep network send/receive work separate from AFE and speaker output tasks.

## Current implementation status

- `AudioService` PCM seam is added by the pinned-upstream patcher.
- Initial Gemini WebSocket transport, API-key handshake, setup gate, JSON/base64 PCM path, transcript forwarding, interruption handling, and protocol selection are implemented in source.
- The local Web Admin now stores Gemini settings in NVS and provides streamed `.bin` OTA to the inactive app partition. OTA rollback validation is performed after the application activation task initializes the selected protocol.
- Bounded reconnect/session resumption, short-lived token provisioning, static compile validation, and hardware validation remain outstanding.
- No firmware build or hardware test has been run.


## Current firmware selection

The firmware selects Gemini only when NVS namespace `gemini` contains `enabled=true` and a non-empty `api_key`. The model defaults to `gemini-3.8-live` and can be overridden with the `model` setting. The optional Vietnamese system instruction is read from NVS key `system_prompt` (NVS key names are limited to 15 characters). Board output is configured for 24 kHz, matching Gemini Live 24 kHz mono PCM. Existing XiaoZhi MQTT/WebSocket selection remains the fallback. The local Web Admin writes these NVS settings. Saving a changed model, prompt, key, or enabled flag requires a device restart before the protocol selection is reloaded.
