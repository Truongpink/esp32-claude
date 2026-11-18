# ChatAI TPIOS — standalone ESP-IDF project

This directory is the independent firmware project. It is deliberately separate
from the existing overlay-based firmware so the known working release path remains
untouched while components are migrated and verified.

## Requirements

- ESP-IDF 6.1
- ESP32-S3 target

## Configure

From the repository root:

```sh
idf.py -C firmware/standalone set-target esp32s3
idf.py -C firmware/standalone reconfigure
```

Build and flash are intentionally not part of this migration step. The Gemini Live
WebSocket client is now connected to startup, but has not been compiled or verified
on hardware. Audio streaming and OTA are not implemented.

## Migration rules

- This project must not fetch, include, or link XiaoZhi source.
- Use native ESP-IDF services and ChatAI-owned modules.
- Keep the existing firmware overlay intact until the standalone replacement has
  passed compile, device, audio, network, and OTA verification.
- Do not define a flash size or partition table until the physical board's current
  flash and partition layout have been read and confirmed.

## Local Web Admin

- The native HTTP server is shared by Wi-Fi provisioning and Web Admin; it remains
  available after the station connects. Open `http://<device-ip>/admin`.
- The page can save, replace, and delete the Gemini API Key in NVS namespace
  `chatai_ai`, key `gemini_key`. Status responses expose only whether a key exists;
  the stored value is never returned to the browser or written to application logs.
- Mutating requests require a same-origin check and a per-boot CSRF token.
- This is a local-network HTTP interface, not an Internet-facing management service.
  HTTP traffic is not encrypted, and the API Key is stored in NVS without encryption
  in the current partition configuration. Do not expose the device admin page to
  untrusted networks.
  The Origin/CSRF checks mitigate cross-site browser requests; they are not user
  authentication. Any client already able to reach the device on the local network
  can open the page and manage the key. A login gate is not part of this first slice.

## Gemini Live connection

- Uses the Gemini Developer API WebSocket endpoint directly with the API Key read
  from NVS. No proxy, Cloudflare Worker, or external server is involved.
- Model: `gemini-3.8-live`; setup requests audio responses and Vietnamese output.
- The client waits for `setupComplete` before reporting the session ready.
- WSS uses ESP-IDF's certificate bundle. Certificate verification is not disabled.
- The client currently establishes the session only. Microphone streaming, speaker
  playback, AEC, text injection, and OTA remain future work.
- The API Key is passed in the WebSocket URL query as required by the Gemini API.
  It is not intentionally written to application logs; transport/library diagnostic
  logging should still be reviewed during device testing.
