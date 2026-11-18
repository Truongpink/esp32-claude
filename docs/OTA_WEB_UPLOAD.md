# Local Web Admin firmware update (OTA)

The Web Admin accepts either a raw ESP-IDF application firmware `.bin` or a `.zip` archive containing that application image.

## Upload flow

1. Open the device Web Admin over its private Wi-Fi LAN.
2. Select the application-only `.bin` or the build ZIP and press **Cài đặt firmware**.
3. For ZIP files, the browser checks the archive and extracts the selected application image locally. It supports ZIP stored and deflate entries; it does not extract files onto the ESP32 filesystem.
4. The browser sends only the resulting application `.bin` as `application/octet-stream` to `POST /api/ota`. The device writes bounded 1 KiB chunks directly to the inactive application partition.
5. The ZIP reader rejects multi-disk and ZIP64 archives, encrypted or unsupported entries, unsafe paths, invalid headers, CRC32 mismatches, oversized firmware, and archives with zero or multiple possible application BIN files. Known boot files (bootloader, boot_app0, partition table, ota_data_initial, USB-FLASH/merged images) are not candidates.
6. The device checks that an inactive OTA application partition exists and that the declared upload length fits its actual partition size.
7. ESP-IDF validates the image with `esp_ota_end`. Only after validation succeeds does the device call `esp_ota_set_boot_partition`.
8. The device responds and restarts. An interrupted upload or failed validation leaves the current boot partition selected.

The ZIP is processed in the phone's browser, not in ESP32 RAM. The archive size is bounded by the Web Admin's ZIP limit (at most 16 MiB); the selected uncompressed application image must fit the actual inactive OTA slot. The firmware uploader does not buffer the complete raw `.bin` in ESP32 RAM.

## Partition layout and rollback

The pinned upstream v2.6.0 `sdkconfig.defaults` enables `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, selects 16 MB flash and `partitions/v2/16m.csv`. That default table contains two 0x3F0000-byte OTA app slots and an 8 MB assets partition. A generated `sdkconfig` may override defaults; the firmware does not hardcode offsets and asks ESP-IDF for the next update partition at runtime.

After the application activation task has completed its asset/version checks and initialized the selected protocol, it marks a pending-verify image valid with `esp_ota_mark_app_valid_cancel_rollback()`. This is later than the Wi-Fi/Web Admin callback and avoids accepting the image before the application has initialized its main services. If startup never reaches that point, ESP-IDF's rollback mechanism can revert to the previous valid image, provided rollback is enabled in the firmware's bootloader configuration and the device has a compatible OTA partition table.

The actual partition table already installed on the user's physical board has not been read back or verified. The first USB/serial installation must use a matching partition table and leave two OTA app slots available. Do not flash an OTA image until the first installation is known to have the correct layout.

## Security and limitations

There is no PIN or login, as requested. Any device able to reach the ESP32 on the same LAN can submit a firmware image and replace the running firmware. The endpoint rejects browser requests with a mismatched Origin, but Origin checking is not authentication and does not stop non-browser clients.

The server uses plain HTTP. The firmware binary is not confidential, but the same server also carries Gemini API key settings. Use only a trusted private network; do not forward port 80 to the Internet or expose the device to an untrusted Wi-Fi network.

The handler verifies image structure through ESP-IDF OTA validation, but it does not cryptographically authenticate a publisher-specific signature. Secure Boot / signed-image enforcement is not configured by this change. Only upload firmware obtained from the trusted ChatAI TPIOS build process.

No firmware build, hardware ZIP/OTA test, GitHub Actions workflow, or flash operation has been run for this change.
