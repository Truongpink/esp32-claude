# Gemini Live authentication and API Key handling

## Chosen architecture

The ESP32-S3 connects directly to Gemini Live. The Gemini API Key is entered by the user in the device's local Web Admin and stored in NVS.

There is no Cloudflare Worker, token broker, VPS, or audio proxy in this architecture.

## Key storage

- NVS namespace: `chatai_ai`
- NVS key: `gemini_key`
- Web Admin accepts a new key through an HTTPS-free local HTTP interface.
- Status responses expose only a boolean indicating whether a key is configured.
- The saved key is not returned to the browser after saving.
- The key must never be printed to OLED, application logs, source code, build configuration, or GitHub.

The current firmware does not enable NVS encryption. Treat physical extraction of the stored key as possible. Keep the device and Web Admin on a trusted local network.

## Gemini Live transport

- Connect directly to the Gemini Live WSS endpoint.
- Use TLS certificate verification with the ESP-IDF certificate bundle.
- Do not disable certificate verification or fall back to plaintext.
- Do not log the credential-bearing WebSocket URL or API Key.
- Wait for `setupComplete` before sending microphone audio.

## Web Admin behavior

- `GET /api/gemini/status` returns only `key_configured`.
- `POST /api/gemini/key` validates and stores a supplied API Key.
- `DELETE /api/gemini/key` deletes the stored API Key.
- Mutating requests require same-origin validation and the per-boot CSRF token.
- The password input is cleared after a save or delete operation.

## Security limitations

The Web Admin uses plain HTTP, so credentials are not encrypted in transit on the local network. Origin and CSRF checks mitigate cross-site browser requests but are not user authentication. Any client that can reach the device on the LAN may manage its API Key.

Do not expose port 80 to the public Internet or an untrusted network. NVS encryption, secure boot, and hardware-backed key protection are not enabled by this change.
