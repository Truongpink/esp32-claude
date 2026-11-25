# Gemini Live wire protocol for ChatAI TPIOS

This document describes the standalone ESP32-S3 client's direct Gemini Live connection.

## Connection and setup

- Use the Gemini Developer API WSS endpoint:
  `wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent`
- Authenticate with the API Key stored in NVS. Never log the full credential-bearing URL or API Key.
- Gemini 3.8 Live uses the documented v1beta WebSocket endpoint.
- Set `generationConfig.responseModalities: ["AUDIO"]` inside `setup.generationConfig` (not directly inside `setup`).
- Include the Vietnamese system instruction in the `setup` message.
- Wait for `setupComplete` before sending microphone audio.
- The configured model is sent as `models/<model-id>`.

## Microphone input

- Mono signed PCM16 at 16 kHz.
- Encode audio as Base64 in `realtimeInput.audio`.
- MIME type: `audio/pcm;rate=16000`.
- Server-side VAD is used as the turn detector; no local VAD is added.

## Speaker output

- Read audio from `serverContent.modelTurn.parts[].inlineData`.
- Accept PCM16 audio at 24 kHz.
- Decode Base64 and queue PCM for the native I2S speaker worker.
- On `serverContent.interrupted`, clear queued speaker audio so stale response audio is not played after an interruption.

## Control and error handling

- `setupComplete`: the session is ready for realtime input.
- `serverContent.turnComplete`: the current response turn is complete.
- `serverContent.interrupted`: stop queued response playback.
- `goAway`, WebSocket close, or transport error: stop microphone forwarding and report the session error; reconnect/session resumption remains future work.

## Diagnostics and credential handling

Diagnostics must not include audio payloads, transcripts, the Gemini API Key, or the credential-bearing WebSocket URL. Error messages should be bounded and should not expose secret material.

## Validation status

The source implements the direct WSS setup gate, PCM16 microphone uplink, PCM16 speaker playback, and interruption handling. A successful compile and physical-device voice test must be recorded separately; source inspection alone is not proof of hardware operation.
