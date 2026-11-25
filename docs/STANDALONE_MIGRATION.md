# ChatAI TPIOS Standalone architecture

## Final architecture decision

The repository now maintains one firmware implementation: `firmware/standalone`, built directly with ESP-IDF 6.1 for ESP32-S3.

The former upstream-download/overlay build path is retired. No third-party Xiaozhi source, protocol, board overlay, cloud endpoint, or build patcher is part of the supported firmware.

## Component ownership

| Area | Owner |
|---|---|
| Wi-Fi provisioning and reconnect | ChatAI native NetworkManager |
| Persistent settings | ESP-IDF NVS |
| OLED | ChatAI native SSD1306 display manager |
| Microphone and speaker | ChatAI native I2S audio manager |
| AI session | Direct Gemini Live WebSocket client |
| API Key configuration | Local Web Admin |
| Firmware build | Manual GitHub Actions Standalone workflow |

## Gemini Live

- Gemini API Key is entered directly in local Web Admin and stored in NVS.
- The ESP32 connects directly to Gemini Live using WSS/TLS.
- The API Key is not displayed after saving and is not intentionally written to logs.
- Microphone audio is mono PCM16 at 16 kHz.
- Speaker audio is mono PCM16 at 24 kHz.
- Server-side VAD is used; local WakeNet, MultiNet, and local VAD are not part of the target architecture.
- No Cloudflare Worker, token broker, VPS, or audio proxy is used.

## Build and output

The only supported build workflow is `.github/workflows/build-standalone.yml`, triggered manually through `workflow_dispatch`. It builds `firmware/standalone` and publishes only:

`ChatAI_TPIOS.bin`

GitHub Actions wraps downloadable artifacts in a ZIP container. The artifact itself contains exactly one application BIN and no bootloader or partition-table files.

## Current limitations

- The current partition layout is single-app; OTA is not implemented.
- Hardware pin assignments remain subject to physical verification.
- A successful CI build is not equivalent to a successful device/audio test.
- The Gemini API Key is stored in NVS without NVS encryption; Web Admin is plain local HTTP and must remain on a trusted LAN.

## Working rules

- Work on `main` only. Never create or switch branches.
- Inspect current branch HEAD and the exact relevant source before every edit.
- Make minimal changes and preserve Wi-Fi, Web Admin, NVS, audio, OLED, TLS, and Gemini behavior.
- Never run or rerun GitHub Actions, builds, or flash operations without explicit permission for that specific operation.
- Never add automatic push/PR workflow triggers.
- Do not claim hardware behavior is verified without an actual device test.
