# Local Web Admin

The ChatAI TPIOS board starts a small HTTP server on port 80 after station Wi-Fi connects and stops it when Wi-Fi disconnects or enters provisioning mode.

- Open `http://<device-ip>/` from a browser on the same LAN.
- `GET /api/status` reports the IP, SSID, Gemini enabled state, and whether an API key is configured. It never returns the key.
- `GET /api/config` returns the model and system prompt, but not the API key.
- `POST /api/config` stores the Gemini settings in NVS. A blank API-key field preserves the previously saved key.
- Configuration is selected when the application initializes its protocol, so the page asks the user to restart the device after saving.

There is intentionally no PIN or login. Anyone with access to the same Wi-Fi network can change the Gemini configuration. The write endpoint rejects browser requests with a mismatched Origin and does not enable CORS, which reduces cross-origin browser abuse but is not authentication. Do not expose port 80 to the public Internet or an untrusted network.

Firmware upload is not included in this first Web Admin step. The pinned upstream has a 16 MB partition table with OTA slots, but the project must confirm the exact build-selected partition table and flash configuration for this board before enabling a firmware write endpoint.
