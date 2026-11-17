# Gemini Live wire protocol for ChatAI TPIOS

This document records the current Gemini Live wire contract for the firmware transport. It is not a claim that the transport has been implemented.

## Connection and setup

- Use a TLS WebSocket (WSS) connection to the Gemini Live API endpoint for the selected supported model.
- Authentication options for raw WebSocket clients:
  - Standard API key: append `?key=<API_KEY>` to the `BidiGenerateContent` endpoint. This is the simplest bring-up path, but the key must never be logged; the upstream WebSocket wrapper currently logs its URL/path at debug level, so the Gemini integration must redact query strings or patch that log before using this option.
  - Ephemeral token (preferred for a deployed device): use the `BidiGenerateContentConstrained` endpoint and send the short-lived token in `Authorization: Token <token>` (or `access_token` query parameter). A token broker must authenticate the local device/user and mint the token using the Gemini AuthToken API. Do not put the long-lived API key in firmware or Web Admin.
- Send exactly one `setup` message after the WebSocket is established.
- Do not send audio until the server returns `setupComplete`.
- Keep the selected model name and endpoint in one configuration location; verify the currently supported model immediately before implementation rather than pinning an old example model.

The exact endpoint and authentication handshake must be checked against the current Google API documentation and account configuration at implementation time.

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

If the actual codec output rate differs from 24 kHz, resample before queueing. The target board profile is expected to use 24 kHz, but this must be confirmed from the initialized codec at runtime.

## Control events

- `setupComplete`: connection is ready for realtime input.
- `serverContent.interrupted == true`: call `ClearPcmPlayback()` immediately and discard any not-yet-queued audio from the interrupted turn.
- `serverContent.turnComplete == true`: mark the logical turn complete; do not assume the hardware playback queue has already drained.
- `goAway`: stop sending new audio and reconnect/resume according to the current API contract.
- `sessionResumptionUpdate`: persist the opaque resumption handle only for the active device session and use it according to the API's expiry rules.
- WebSocket close/error: stop mic forwarding, clear stale audio, apply bounded exponential reconnect backoff, then establish a new setup handshake.

## Memory and task rules

- The receive callback must only parse/copy bounded payloads and enqueue work; it must not call blocking codec APIs.
- Never buffer an entire conversation or an unbounded audio stream in RAM.
- Base64 temporarily increases payload size; size all message buffers for the encoded form and reject oversized frames. The current WebSocket wrapper rejects outgoing payloads over 65,535 bytes and logs the URL path at debug level; never put credentials in logged URL text.
- Do not log API keys, bearer tokens, full setup URLs containing credentials, or audio payloads.
- Keep network send/receive work separate from AFE and speaker output tasks.

## Current implementation status

- `AudioService` PCM seam is added by the pinned-upstream patcher.
- The Gemini WebSocket transport, authentication, setup handshake, JSON/base64 parser, resampling guard, reconnect/session-resumption, and application state integration are **not implemented yet**.
- No firmware build or hardware test has been run.
