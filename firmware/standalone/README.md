# ChatAI TPIOS — standalone ESP-IDF project

This is the only firmware implementation maintained in this repository. It is an independent ESP-IDF application and does not fetch, include, or link any third-party Xiaozhi firmware or cloud components.

## Requirements

- ESP-IDF 6.1
- ESP32-S3 target

## Configure and build

From the repository root:

```sh
idf.py -C firmware/standalone set-target esp32s3
idf.py -C firmware/standalone reconfigure
idf.py -C firmware/standalone build
```

The application image is generated at `build/ChatAI_TPIOS.bin` (relative to this directory).

## Firmware scope

- Native ESP-IDF initialization, Wi-Fi provisioning, reconnect handling, and NVS settings.
- SSD1306 OLED status display.
- Native I2S microphone and speaker PCM interfaces.
- Direct Gemini Live WebSocket connection over WSS/TLS.
- Local Web Admin for Gemini API Key management and application-only OTA firmware updates.

## Gemini credentials

Enter the Gemini API Key through the local Web Admin. The key is stored in NVS namespace `chatai_ai`, key `gemini_key`. Status responses expose only whether a key is configured. The key is not returned to the browser or intentionally written to application logs.

The current design intentionally uses the direct Gemini API Key path. There is no Cloudflare Worker, token broker, VPS, or audio proxy.

## Audio and Gemini Live

- Microphone: mono PCM16, 16 kHz.
- Speaker: mono PCM16, 24 kHz.
- The client waits for `setupComplete` before sending microphone audio.
- Server-side VAD is used; no local WakeNet, MultiNet, or local VAD is added.
- TLS certificate verification uses the ESP-IDF certificate bundle.

The implementation still requires a fresh authorized build and physical-device validation. Do not describe microphone capture, speaker output, or a complete voice conversation as hardware-verified until tested on the actual board.

## Flash layout and OTA

The defaults select the 16 MB dual-slot partition layout used by the earlier Xiaozhi-derived ChatAI OTA firmware: `ota_0` and `ota_1` are each `0x3F0000` bytes, with `otadata` and the existing NVS offset preserved. The standalone Web Admin accepts one application-only `ChatAI_TPIOS.bin` and writes it to the inactive OTA slot. The new app confirms successful startup with ESP-IDF rollback support.

The user confirmed the board variant is N16R8 (16 MB flash / 8 MB PSRAM). The exact partition table currently installed on the physical unit has not been read back; compatibility is based on the previously deployed project layout and the user's successful OTA history. Keep the existing bootloader and partition table during the application-only transition.

## Security

The Web Admin is a local HTTP interface, and NVS encryption is not enabled in the current configuration. Use it only on a trusted LAN. Do not expose it to the public Internet. Never print the API Key to OLED, logs, source code, or GitHub.
