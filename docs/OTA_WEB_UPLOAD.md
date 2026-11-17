# Local Web Admin firmware update (OTA)

The Web Admin accepts a raw ESP-IDF application firmware `.bin` file. It does not accept a merged image containing bootloader/partition table, nor an arbitrary binary.

## Upload flow

1. Open the device Web Admin over its private Wi-Fi LAN.
2. Select the ChatAI TPIOS application `.bin` and press **Nạp firmware**.
3. The browser streams the file as `application/octet-stream` to `POST /api/ota`; the device writes bounded 1 KiB chunks directly to the inactive application partition.
4. The handler checks that an inactive OTA application partition exists and that the declared upload length fits its actual partition size.
5. ESP-IDF validates the image with `esp_ota_end`. Only after validation succeeds does the device call `esp_ota_set_boot_partition`.
6. The device responds and restarts. An interrupted upload or failed validation leaves the current boot partition selected.

The uploader does not buffer the complete firmware in RAM. The file must fit the inactive slot. The upstream default slot size is 0x3F0000 bytes (4,128,768 bytes, approximately 3.94 MiB); the handler checks the actual target partition size at runtime and does not assume this default.

## Partition layout and rollback

The pinned upstream v2.6.0 `sdkconfig.defaults` enables `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, selects 16 MB flash and `partitions/v2/16m.csv`. That default table contains two 0x3F0000-byte OTA app slots and an 8 MB assets partition. A generated `sdkconfig` may override defaults; the firmware does not hardcode offsets and asks ESP-IDF for the next update partition at runtime.

After board/audio initialization, Wi-Fi connection and successful registration of the local Web Admin routes, the firmware marks a pending-verify image valid with `esp_ota_mark_app_valid_cancel_rollback()`. If startup never reaches that point, ESP-IDF's rollback mechanism can revert to the previous valid image, provided rollback is enabled in the firmware's bootloader configuration and the device has a compatible OTA partition table.

The actual partition table already installed on the user's physical board has not been read back or verified. The first USB/serial installation must use a matching partition table and leave two OTA app slots available. Do not flash an OTA image until the first installation is known to have the correct layout.

## Security and limitations

There is no PIN or login, as requested. Any device able to reach the ESP32 on the same LAN can submit a firmware image and replace the running firmware. The endpoint rejects browser requests with a mismatched Origin, but Origin checking is not authentication and does not stop non-browser clients.

The server uses plain HTTP. The firmware binary is not confidential, but the same server also carries Gemini API key settings. Use only a trusted private network; do not forward port 80 to the Internet or expose the device to an untrusted Wi-Fi network.

The handler verifies image structure through ESP-IDF OTA validation, but it does not cryptographically authenticate a publisher-specific signature. Secure Boot / signed-image enforcement is not configured by this change. Only upload firmware obtained from the trusted ChatAI TPIOS build process.

No build, hardware OTA test, GitHub Actions workflow, or flash operation has been run.
