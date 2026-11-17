# Local Web Admin

The device has two distinct local web entry points, depending on its Wi-Fi mode:

- **Wi-Fi setup (AP mode):** hold the BOOT button for 2 seconds after startup. Connect the phone to `ChatAI-TPIOS-<suffix>` and open `http://192.168.4.1/`. This is the upstream captive portal for scanning and saving the home Wi-Fi credentials.
- **Device administration (station mode):** after the ESP32 joins the home Wi-Fi, its setup AP is stopped. Open `http://<device-ip>/` from a phone or computer on the same LAN. The IP is shown by `GET /api/status` and can also be found in the router's connected-device list.

The two addresses are not interchangeable: `192.168.4.1` is the ESP32's AP address and is only available while setup AP mode is active. In station mode, use the LAN IP assigned by the router.

The ChatAI TPIOS board starts a small HTTP server on port 80 after station Wi-Fi connects and stops it when Wi-Fi disconnects or enters provisioning mode.

- Open `http://<device-ip>/` from a browser on the same LAN.
- `GET /api/status` reports the IP, SSID, Gemini enabled state, and whether an API key is configured. It never returns the key.
- `GET /api/config` returns the model and system prompt, but not the API key.
- `POST /api/config` stores the Gemini settings in NVS. A blank API-key field preserves the previously saved key.
- Configuration is selected when the application initializes its protocol, so the page asks the user to restart the device after saving.

There is intentionally no PIN or login. Anyone with access to the same Wi-Fi network can change the Gemini configuration. The write endpoint rejects browser requests with a mismatched Origin and does not enable CORS, which reduces cross-origin browser abuse but is not authentication. Do not expose port 80 to the public Internet or an untrusted network.

Firmware upload is not included in this first Web Admin step. The pinned upstream has a 16 MB partition table with OTA slots, but the project must confirm the exact build-selected partition table and flash configuration for this board before enabling a firmware write endpoint.


The server uses plain HTTP on port 80. The API key therefore travels unencrypted over the local Wi-Fi connection when first saved or changed. Use only a trusted private Wi-Fi network; do not expose this port to the public Internet or an untrusted network. NVS storage is not a substitute for enabling ESP-IDF NVS encryption.

The pinned upstream `sdkconfig.defaults` selects the 16 MB `partitions/v2/16m.csv` layout, which has two OTA app slots and an 8 MB assets partition. A pre-existing generated `sdkconfig` can override defaults, and the physical device's installed partition table is not verified. Confirm the actual build and device layout before enabling firmware upload.
