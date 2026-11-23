# ChatAI TPIOS local Web Admin

The Standalone firmware provides a responsive mobile Web Admin for Wi-Fi provisioning, Gemini API Key, Gemini Live voice and System Prompt settings, speaker volume, device telemetry, diagnostics, and OTA updates.

## Access

- After the device joins the same home Wi-Fi network as the phone or computer, open `http://chatai.local/` or `http://chatai.local/admin`.
- The `.local` address is provided by mDNS; a fixed DHCP address is not required.
- If the router or client network blocks mDNS multicast, use the device's current LAN IP as a fallback.
- During Wi-Fi provisioning, connect to the open setup access point `ChatAI-TPIOS-XXXXXX` and open `http://192.168.50.1/`. Captive DNS redirects HTTP connectivity checks to the setup page.

## Gemini API Key controls

The Web Admin supports:

- Scan nearby Wi-Fi networks and connect to a selected SSID; new credentials are committed only after Internet verification.
- View whether an API Key is configured (the key value is never returned).
- Save or replace a Gemini API Key.
- Delete the saved Gemini API Key.
- Adjust speaker output volume (0–100); the value is persisted in NVS and applied at audio initialization.
- Adjust digital microphone input gain (-6 to +6 dB) from the Audio tab; the value is persisted in NVS and applied to PCM before AFE/AGC. The default is 0 dB. Increasing gain can cause clipping when the raw microphone signal already approaches full scale.
- Select a Gemini Live prebuilt voice and persist it in NVS.
- Edit and persist the Gemini Live System Prompt; the current firmware limit is 3,000 UTF-8 bytes because the deployed NVS partition is 16 KB.
- Use built-in editable prompt templates for personal assistant, companion, technology expert, coding assistant, language teacher, and customer support.
- View Wi-Fi SSID/RSSI, IP, gateway, DNS, Internet probe state, SoftAP address, Gemini/TLS state, free/minimum heap, uptime, firmware build, and speaker volume on the dashboard.
- Display the running firmware Build Number and OTA slot capacity.
- Upload an application-only `.bin` file or a `.zip` containing exactly one application BIN. ZIP extraction and CRC32 validation happen in the browser; the ESP32 receives only the BIN.
- Show file name, size, target Build Number, upload percentage, flash/validation stage, and the actual Build Number after reboot.

The key is stored in NVS namespace `chatai_ai`, key `gemini_key`. The browser input is cleared after saving or deleting. A blank key is not returned as a substitute for the saved value.

OTA updates do not erase the NVS namespaces used by Wi-Fi (`chatai_net`), Gemini (`chatai_ai`), or speaker volume (`chatai_cfg`). Saved Wi-Fi credentials and the Gemini API Key are retained across OTA. No full NVS or flash erase is performed.

## HTTP endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/admin` | Web Admin page |
| GET | `/api/wifi/scan` | Scans nearby Wi-Fi networks while connected or provisioning |
| GET | `/api/wifi/status` | Returns Wi-Fi connection/provisioning state |
| POST | `/api/wifi/connect` | Starts a Wi-Fi connection; commits credentials after Internet verification |
| GET | `/api/gemini/status` | Returns only whether a key is configured |
| GET | `/api/gemini/settings` | Returns saved voice and System Prompt; never returns the API Key |
| POST | `/api/gemini/voice` | Validates and saves a supported Live API prebuilt voice |
| POST | `/api/gemini/prompt` | Validates and saves the System Prompt (up to 3,000 UTF-8 bytes) |
| GET | `/api/device/status` | Returns live network, Gemini, heap, uptime, firmware, and audio telemetry |
| POST | `/api/audio/volume` | Saves speaker output volume (0–100) in NVS |
| POST | `/api/audio/mic-gain` | Saves microphone digital input gain (-6 to +6 dB) in NVS |
| GET | `/api/firmware/status` | Returns running Build Number, marker, active slot, inactive OTA slot, and slot capacity |
| POST | `/api/gemini/key` | Validate and save a key |
| DELETE | `/api/gemini/key` | Delete the saved Gemini API Key |
| POST | `/api/ota` | Stream and validate an application BIN into the inactive OTA slot |

Mutating requests require same-origin validation and the per-boot CSRF token. The browser accepts BIN or ZIP, but always uploads only the extracted raw application BIN. ZIP64, multi-disk archives, encrypted BINs, unsafe paths, multiple BIN candidates, unsupported compression, invalid size, and CRC32 mismatches are rejected before upload. The ESP32 rejects images larger than the inactive OTA slot and changes the boot partition only after `esp_ota_end()` validates the image.

## Current feature boundaries

The current Standalone firmware does not expose bot name, language selection, wake-word controls, or OLED idle-mode selection. Gemini Live voice preview is not implemented: the current Live API setup accepts the selected voice when a session starts, but the firmware has no independent preview endpoint or safe preview playback path. These controls are intentionally omitted from the Web Admin rather than presented as non-functional switches. OLED save notifications are supported for Wi-Fi, Gemini API Key, and speaker volume. A separate sound-confirmation tone is not enabled because the speaker is shared with Gemini Live playback and there is no independent notification buzzer/mixer path. Saving a voice or System Prompt sends an OLED notification and requests a new Gemini Live session when an API Key is configured.

## Security

The Web Admin uses plain HTTP and is not protected by a login. Origin and CSRF checks are not user authentication. Any client able to reach the device on the LAN can manage the Gemini API Key.

NVS encryption is not enabled in the current partition configuration. Do not expose the Web Admin to the Internet or an untrusted Wi-Fi network. The API Key must never be written to logs, OLED, source code, or GitHub.

## Firmware build

The manual-only Standalone workflow builds `firmware/standalone` and publishes one file: `ChatAI_TPIOS.bin`. A normal build reuses the latest official Build Number. Selecting the explicit `publish_release` input computes the next number from published `build-N` GitHub releases and creates that release only after the build and slot-size checks pass. Failed build attempts do not consume a Build Number. It does not package bootloader or partition-table binaries.

GitHub Actions downloadable artifacts are wrapped in a ZIP container by the platform. The downloaded ZIP contains only the single application BIN.
