# Local Web Admin

The device has two local web entry points:

- **Wi-Fi setup (AP mode):** hold BOOT for 2 seconds. Connect to `ChatAI-TPIOS-<suffix>` and open `http://192.168.4.1/`. The page uses the pinned provisioning server's existing `/scan`, `/submit`, and `/exit` handlers.
- **Device administration (station mode):** after joining the home Wi-Fi, open `http://<device-ip>/` from a device on the same LAN. The current IP is shown by `GET /api/status` and in the router's client list.

The AP address and station address are different. `192.168.4.1` is available only while the setup AP is active. After Wi-Fi setup, use the router-assigned LAN IP.

## Supported controls

- `GET /api/status`: Wi-Fi SSID/IP, Gemini enabled state, API-key configured flag, firmware version, running partition, inactive OTA target/size, and saved speaker volume. The API key itself is never returned.
- The **Chẩn đoán Gemini Live** panel in the device page shows the current reset reason, Live session/microphone state, sent microphone chunk count, received response-audio chunk count, and last Gemini error. It can be refreshed and copied from the browser. These are current-session diagnostics, not a persistent log or a PANIC backtrace.
- `GET /api/config`: Gemini model, system prompt, enabled state, and speaker volume.
- `POST /api/config`: saves Gemini model/prompt/enabled/API key and speaker volume in NVS. An omitted or blank API key preserves the saved key. Gemini changes take effect after reboot; speaker volume is applied immediately and restored at startup.
- `POST /api/ota`: accepts one raw application-only firmware `.bin` body as `application/octet-stream`. The handler checks that the dual OTA slots and OTA data partition exist, limits the upload to the inactive slot, streams data to flash, validates the image with ESP-IDF OTA APIs, selects the new boot partition, then restarts.

The firmware page follows the original dark/purple AI Voice Bot layout. Wi-Fi controls are shown in AP mode. Gemini, speaker volume, device status, and OTA controls are shown in station mode. Microphone gain, wake-word enablement, and OLED display modes are not exposed as active controls because the current board firmware does not yet implement configuration APIs for them.

## OTA image requirements

Upload only the application image named `ChatAI-TPIOS-v0.1.1-OTA.bin` from the OTA artifact. Do **not** upload `ChatAI-TPIOS-v0.1.1-USB-FLASH.bin`: the USB image includes bootloader/partition data and is intended for first-time USB flashing, not the app OTA partition.

The pinned 16 MB partition layout defines two 0x3F0000-byte app slots (`ota_0` and `ota_1`) plus the `otadata` partition. The handler also checks the actual partition table at runtime before accepting an upload. The image must fit the inactive slot. A successful upload selects the inactive slot and reboots.

ESP-IDF rollback is enabled. On the next boot, the patched activation task marks the new app valid only after the application has initialized its selected protocol without a startup error. If the device resets before validation, the bootloader can roll back to the previous slot. This startup check does not prove that a later Gemini cloud session will succeed.

## Security and recovery

There is no PIN/login. Anyone on the same trusted LAN can change configuration or upload firmware. The browser write endpoints reject mismatched Origin headers, but this is not authentication. The server uses plain HTTP, so the API key is not encrypted while being sent over Wi-Fi. Do not expose port 80 to the public Internet or an untrusted network.

Keep the USB flash image from the same release as a recovery path. Do not remove power during OTA. If an upload fails validation, the running firmware remains selected. If the new firmware fails during startup validation, ESP-IDF rollback should restore the previous app slot.

## Build flow

The manual-only GitHub Actions workflow prepares the pinned XiaoZhi source, patches the AP portal asset, reconfigures, and builds. It publishes two separate artifacts:

- `ChatAI-TPIOS-v0.1.1-USB-FLASH`: merged image for USB flashing.
- `ChatAI-TPIOS-v0.1.1-OTA`: application-only image for Web Admin OTA.

The workflow remains `workflow_dispatch` only. It is not triggered by push or pull requests.
