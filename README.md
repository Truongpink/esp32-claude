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
3. Download the versioned `ChatAI-TPIOS-N` artifact.

The workflow checks out the latest `main` commit and records its SHA in the workflow log and release notes. It builds the next Build Number after the latest published `build-N` release. The Actions artifact contains only `ChatAI-TPIOS-N.bin` (GitHub packages the artifact as a downloadable ZIP). The GitHub Release publishes both `ChatAI-TPIOS-N.bin` and `ChatAI-TPIOS-N.zip`; the release ZIP contains exactly one file, `ChatAI-TPIOS-N.bin`. The workflow verifies the ESP application image format, ZIP integrity and contents, and the 16 MB OTA slot size before publishing.

The BIN is the application image only. The device's local Web Admin accepts either the BIN directly or the matching ZIP and extracts the BIN before upload. ESP32 confirms installation only after the new image boots and the bootloader accepts it; Web Admin records the actual BIN filename only after that confirmation. If confirmation is not received, the previously recorded filename remains unchanged. The standalone firmware targets the existing 16 MB dual-slot layout; the installed bootloader and partition table are retained during application-only OTA.

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

- Standalone uses the compatible 16 MB dual-slot OTA partition table and local Web Admin application-only OTA upload. The actual table installed on the physical board has not been read back independently.
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


## ECC Shin engineering workflow

This repository includes **ECC Shin**, its project-specific engineering governance and documentation workflows.

- [ECC Shin Core](ECC-SHIN/CORE/ECC-SHIN-CORE.md)
- [12 specialist roles](ECC-SHIN/CORE/AGENT-ROLES.md)
- [10-skill catalog](ECC-SHIN/CORE/SKILL-CATALOG.md)
- [Security policy](ECC-SHIN/CORE/SECURITY-POLICY.md)
- [Project memory](ECC-SHIN/PROJECT/PROJECT.md)
- [Architecture memory](ECC-SHIN/PROJECT/ARCHITECTURE.md)
- [Technical decisions](ECC-SHIN/PROJECT/TECHNICAL-DECISIONS.md)
- [Progress](ECC-SHIN/PROJECT/PROGRESS.md)
- [Issues and handoff](ECC-SHIN/PROJECT/ISSUES-HANDOFF.md)

Start with `AGENTS.md` and `ECC-SHIN/CORE/ECC-SHIN-CORE.md` before performing project work. ECC Shin is a documented set of specialist workflows coordinated by the assistant; it does not claim to run independent autonomous agents. All existing repository safety rules remain in force. Changes require explicit approval; Actions/build/flash/OTA/deployment require separate authorization.
