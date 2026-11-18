# ChatAI TPIOS local Web Admin

The Standalone firmware provides a local Web Admin for configuring the Gemini API Key on the ESP32-S3.

## Access

- During Wi-Fi provisioning, connect to the device's setup access point and open `http://192.168.4.1/`.
- After the device joins the home Wi-Fi, open the device's LAN address and navigate to `/admin`.
- Keep the device and browser on a trusted local network.

## Gemini API Key controls

The Web Admin supports:

- View whether an API Key is configured (the key value is never returned).
- Save or replace a Gemini API Key.
- Delete the saved Gemini API Key.
- Display the running firmware Build Number and OTA slot capacity.
- Upload an application-only `.bin` file or a `.zip` containing exactly one application BIN. ZIP extraction and CRC32 validation happen in the browser; the ESP32 receives only the BIN.
- Show file name, size, target Build Number, upload percentage, flash/validation stage, and the actual Build Number after reboot.

The key is stored in NVS namespace `chatai_ai`, key `gemini_key`. The browser input is cleared after saving or deleting. A blank key is not returned as a substitute for the saved value.

## HTTP endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/admin` | Web Admin page |
| GET | `/api/gemini/status` | Returns only whether a key is configured |
| GET | `/api/firmware/status` | Returns running Build Number, marker, active slot, inactive OTA slot, and slot capacity |
| POST | `/api/gemini/key` | Validate and save a key |
| DELETE | `/api/gemini/key` | Delete the saved key |
| POST | `/api/ota` | Stream and validate an application BIN into the inactive OTA slot |

Mutating requests require same-origin validation and the per-boot CSRF token. The browser accepts BIN or ZIP, but always uploads only the extracted raw application BIN. ZIP64, multi-disk archives, encrypted BINs, unsafe paths, multiple BIN candidates, unsupported compression, invalid size, and CRC32 mismatches are rejected before upload. The ESP32 rejects images larger than the inactive OTA slot and changes the boot partition only after `esp_ota_end()` validates the image.

## Security

The Web Admin uses plain HTTP and is not protected by a login. Origin and CSRF checks are not user authentication. Any client able to reach the device on the LAN can manage the Gemini API Key.

NVS encryption is not enabled in the current partition configuration. Do not expose the Web Admin to the Internet or an untrusted Wi-Fi network. The API Key must never be written to logs, OLED, source code, or GitHub.

## Firmware build

The manual-only Standalone workflow builds `firmware/standalone` and publishes one file: `ChatAI_TPIOS.bin`. A normal build reuses the latest official Build Number. Selecting the explicit `publish_release` input computes the next number from published `build-N` GitHub releases and creates that release only after the build and slot-size checks pass. Failed build attempts do not consume a Build Number. It does not package bootloader or partition-table binaries.

GitHub Actions downloadable artifacts are wrapped in a ZIP container by the platform. The downloaded ZIP contains only the single application BIN.
