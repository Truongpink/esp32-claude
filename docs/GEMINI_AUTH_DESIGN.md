# Gemini Live authentication design

## Decision

The ESP32-S3 must not contain the long-lived Gemini API key. The standalone firmware connects directly to Gemini Live over WSS, but obtains a short-lived, constrained ephemeral token from a ChatAI token broker first.

- ChatAI token broker stores the long-lived Gemini API key as a server-side secret and calls Google's AuthTokenService.
- ESP32-S3 authenticates to the broker, receives a short-lived token, then connects directly to Gemini Live.
- Google Gemini Live receives audio and returns audio over the device's WSS session.

The broker is an authentication service only; it must not proxy microphone audio or model responses.

## Why not store the Gemini API key in device NVS?

The current standalone project has no verified flash-encryption or NVS-encryption configuration. Its Wi-Fi credentials are currently stored in NVS without encryption. Adding a Gemini API key to the same storage would expose a long-lived, billable credential to physical firmware/flash extraction.

Do not hardcode a Gemini API key in source, build definitions, sdkconfig, or firmware assets; commit keys or tokens; log request headers, API keys, ephemeral tokens, or authenticated URLs; put long-lived credentials in the local Wi-Fi provisioning form or OLED; or enable flash/NVS encryption before the actual partition table, provisioning lifecycle, recovery process, and board security state are verified.

## Proposed request flow

1. Device connects to Wi-Fi using the existing NetworkManager.
2. Device requests a short-lived broker challenge or uses a previously enrolled device identity.
3. Device authenticates to the broker over HTTPS. The device credential must be unique per device, revocable, and scoped only to token issuance. Do not use a shared fleet-wide secret.
4. Broker validates device identity, rate limits requests, and calls Google's AuthTokenService using its server-side Gemini API key.
5. Broker creates a single-use ephemeral token with a short expiry and live_connect_constraints restricting the permitted model and Live session configuration.
6. Broker returns only the ephemeral token and its expiry to the device.
7. Device opens the Google constrained Live WebSocket endpoint using the ephemeral token. Prefer the Authorization: Token <token> handshake header over a query parameter to reduce accidental URL logging.
8. Device sends the Gemini Live setup message and waits for setupComplete before streaming audio.
9. Device discards the token from RAM after the session has been established; it never persists it to NVS or logs it.

The token broker must not accept arbitrary model names or unrestricted Live configurations from the device. It should enforce an allowlist and fixed session constraints.

## Device identity and enrollment

Enrollment is not implemented yet. Before coding it, choose and document the bootstrap process.

Required properties: unique per-device identity; explicit owner-controlled enrollment/pairing; revocation and re-enrollment; challenge-response or signed request with replay protection; server-side rate limiting and per-device quotas; no credential shown on OLED, diagnostic logs, or Wi-Fi setup page.

A random secret stored in ordinary NVS is not equivalent to hardware-backed secure storage. Treat it as extractable unless a verified hardware-backed protection mechanism is implemented.

## TLS and WebSocket requirements

- Use WSS only.
- Enable server certificate verification. Espressif documents that when no server certificate is configured, TLS does not require verification by default. Use the ESP-IDF certificate bundle or a managed CA strategy; never skip verification.
- Send the ephemeral token in the initial WebSocket handshake Authorization: Token <token> header if supported by the pinned component version. Never log headers.
- Keep TLS verification failures fatal; do not fall back to plaintext or insecure TLS.
- Bound token response size, validate expiry, and reject empty or malformed tokens.
- Do not automatically retry a consumed single-use token. Request a fresh token from the broker after a failed or closed connection.

## Operational controls

The broker must keep the Gemini API key in a server-side secret manager/environment secret; restrict the key to Gemini API use where supported; issue single-use tokens with short expiry and fixed Live constraints; rate limit by device and globally; redact secrets from logs and error traces; provide device revocation and usage monitoring; and avoid storing audio or transcripts unless separately required.

## Current blockers

- No token broker exists in this repository.
- Device enrollment/authentication is not designed or implemented.
- No verified secure device credential storage exists.
- TLS certificate verification configuration for the standalone project has not been implemented.
- The exact Gemini Live model and allowed session configuration must be verified before token constraints are finalized.

Therefore, do not implement the Gemini WebSocket session client or expose a local API-key entry field yet. The next implementation step is the broker/enrollment contract and deployment choice, followed by TLS verification and the device token client.

## References

- Google Gemini Live API reference: ephemeral tokens via AuthTokenService and the constrained Live WebSocket endpoint.
- Google ephemeral-token guide: short-lived, constrained credentials for client-to-server Live connections.
- Espressif ESP WebSocket Client documentation: WSS configuration, handshake headers, and certificate verification behavior.
