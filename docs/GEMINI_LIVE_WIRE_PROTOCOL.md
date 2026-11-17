# Gemini Live wire protocol for ChatAI TPIOS

This document records the Gemini Live wire contract and the implementation boundaries in the current firmware.

## Connection and setup

- Use a TLS WebSocket (WSS) connection to the Gemini Developer API endpoint:
  `wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent`.
- The current firmware authenticates with the API key in the URL query string (`?key=...`). Never log the full URL or API key. A future production design should use short-lived ephemeral tokens.
- Send exactly one `setup` message after the WebSocket is established.
- Do not send audio until the server returns `setupComplete`.
- The default model is `gemini-3.8-live`; it can be changed through Web Admin/NVS.

## Setup message

The setup message requests audio output, the configured system instruction, and input/output transcription. The model name is sent as `models/<model-id>`.

## Microphone input

The AFE bridge supplies signed PCM16 mono at 16 kHz. Each bounded frame is Base64-encoded into `realtimeInput.audio` with MIME type `audio/pcm;rate=16000`. Server-side automatic VAD is used initially.

## Speaker output

For each received JSON server message:

1. Parse `serverContent.modelTurn.parts[]`; a message can contain more than one part.
2. For every `inlineData` audio part, require PCM at 24 kHz.
3. Base64-decode signed PCM16 little-endian samples.
4. Push bounded chunks to `AudioService::PushPcmPlayback()`.
5. Keep the WebSocket receive callback away from blocking codec operations.

## Control events

- `setupComplete`: the session is ready for realtime input.
- `serverContent.interrupted == true`: clear queued speaker audio immediately.
- `serverContent.turnComplete == true`: end the logical response turn.
- `goAway`: the server is preparing to close the session; reconnect/resumption remains future work.
- WebSocket close/error: stop microphone forwarding and clear stale speaker audio.

## Receive diagnostics

Gemini Live server messages are JSON envelopes. The receive callback records the frame kind (text/binary), byte count, and at most the first eight bytes in hexadecimal when parsing fails; it never logs the full message, transcript, audio payload, API key, or credential-bearing URL.

The callback attempts JSON parsing even if the transport labels the frame as binary. This distinguishes a WebSocket opcode mismatch from a genuinely non-JSON binary payload. Empty, oversized, non-object JSON, and transport errors have separate diagnostic messages instead of one generic error.

## Current implementation

- 16 kHz mono PCM16 microphone uplink and 24 kHz mono PCM16 speaker playback.
- WebSocket setup gate, JSON/Base64 audio path, transcription forwarding, interruption handling, and Gemini protocol selection.
- Web Admin stores Gemini settings in NVS.
- OTA firmware update is handled by the local Web Admin endpoint; it writes only to the inactive OTA app partition and relies on ESP-IDF image validation and boot rollback.
- Bounded reconnect/session resumption and short-lived token provisioning are not implemented.
- Hardware validation of Gemini conversation and audio capture remains pending.
