# ChatAI TPIOS — ESP32-S3

Independent ChatAI TPIOS firmware built directly with ESP-IDF 6.1. The firmware owns Wi-Fi, audio I/O, OLED, local Web Admin, NVS settings, and the direct Gemini Live connection.

## Architecture

- Target: ESP32-S3.
- Firmware project: `firmware/standalone`.
- AI service: Gemini Live over WSS/TLS; no Xiaozhi source, server, proxy, or token broker.
- Gemini API Key: entered in the device's local Web Admin and stored in NVS. The saved key is never returned by the status endpoint or intentionally written to logs.
- Audio: mono PCM16 microphone input at 16 kHz and speaker output at 24 kHz.
- Web Admin: local device interface for saving, replacing, and deleting the Gemini API Key.
- Display: SSD1306 128×64, I²C address 0x3C.

## Build

The only supported repository build is the manual Standalone workflow:

1. Open **GitHub → Actions → Build ChatAI TPIOS standalone firmware**.
2. Select **Run workflow** on `main`.
3. Download the `ChatAI_TPIOS` artifact.

The artifact contains exactly one file: `ChatAI_TPIOS.bin`. GitHub Actions packages downloadable artifacts as ZIP files automatically, so the downloaded container is a ZIP, but it contains no bootloader, partition table, or other files.

The BIN is the application image only. It is intended for OTA through the device's local Web Admin. The standalone firmware now targets the existing 16 MB dual-slot layout used by the earlier OTA firmware; the installed bootloader and partition table are retained during application-only OTA.

## Local build

With ESP-IDF 6.1 installed:

```sh
idf.py -C firmware/standalone set-target esp32s3
idf.py -C firmware/standalone reconfigure
idf.py -C firmware/standalone build
```

The application image is generated at:

`firmware/standalone/build/ChatAI_TPIOS.bin`

## Current limitations

- The current partition configuration is a single-app layout; OTA is not implemented in this Standalone firmware.
- Physical board pin mappings, microphone capture, speaker playback, and a complete Gemini Live conversation still require validation on the actual device.
- The local Web Admin uses HTTP and the Gemini API Key is stored in NVS without NVS encryption. Use only on a trusted local network; do not expose the admin page to the Internet.
- Do not assume flash size or hardware pin mappings beyond the configuration documented in `docs/HARDWARE.md`.

## Project documentation

- [Current project checkpoint](docs/CHECKPOINT.md)
- [Standalone architecture and migration status](docs/STANDALONE_MIGRATION.md)
- [Gemini Live wire protocol](docs/GEMINI_LIVE_WIRE_PROTOCOL.md)
- [Gemini authentication and key handling](docs/GEMINI_AUTH_DESIGN.md)
- [Hardware profile and unverified pins](docs/HARDWARE.md)
- [Local Web Admin](docs/WEB_ADMIN.md)

GitHub Actions remains manual-only. Never add automatic push/PR triggers, and do not run builds or flash hardware without explicit authorization.
