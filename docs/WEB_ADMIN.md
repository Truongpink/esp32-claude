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
- Upload one application-only `.bin` file to the inactive OTA slot, validate it, select it for the next boot, and restart.

The key is stored in NVS namespace `chatai_ai`, key `gemini_key`. The browser input is cleared after saving or deleting. A blank key is not returned as a substitute for the saved value.

## HTTP endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/admin` | Web Admin page |
| GET | `/api/gemini/status` | Returns only whether a key is configured |
| POST | `/api/gemini/key` | Validate and save a key |
| DELETE | `/api/gemini/key` | Delete the saved key |

Mutating requests require same-origin validation and the per-boot CSRF token.

## Security

The Web Admin uses plain HTTP and is not protected by a login. Origin and CSRF checks are not user authentication. Any client able to reach the device on the LAN can manage the Gemini API Key.

NVS encryption is not enabled in the current partition configuration. Do not expose the Web Admin to the Internet or an untrusted Wi-Fi network. The API Key must never be written to logs, OLED, source code, or GitHub.

## Firmware build

The manual-only Standalone workflow builds `firmware/standalone` and publishes one file: `ChatAI_TPIOS.bin`. It does not package bootloader or partition-table binaries.

GitHub Actions downloadable artifacts are wrapped in a ZIP container by the platform. The downloaded ZIP contains only the single application BIN.
