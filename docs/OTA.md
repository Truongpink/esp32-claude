# ChatAI TPIOS local OTA

## Which file to upload

Use the application-only image from the `ChatAI-TPIOS-v0.1.2-OTA` Actions artifact:

`ChatAI-TPIOS-v0.1.2-OTA.bin`

Do not use the merged `USB-FLASH.bin` image for OTA. The merged image contains bootloader and partition-table data and is intended for USB flashing.

## Update procedure

1. Run the manual GitHub Actions workflow; it builds and publishes only the versioned OTA application image (no merged USB image).
2. Download the `ChatAI-TPIOS-v0.1.2-OTA` artifact ZIP. You can upload this ZIP directly in Web Admin, or extract it and select the `.bin` file.
3. Connect the ESP32 to the same trusted Wi-Fi network as the phone.
4. Open `http://<device-ip>/`.
5. In **Cập nhật Firmware OTA**, choose the application-only `.bin` file or the ZIP archive and start the update. ZIP files are checked and extracted in the browser; only the application `.bin` is sent to the ESP32.
6. Keep the board powered until it restarts and the Web Admin becomes reachable again.

The firmware streams the upload into the inactive app slot. It verifies the partition layout, rejects images larger than the inactive slot, calls `esp_ota_end()` to validate the image, and selects the new slot only after validation succeeds.

## Rollback

The pinned 16 MB layout includes `ota_0`, `ota_1`, and `otadata`. ESP-IDF bootloader rollback is enabled. The ChatAI patch marks a pending image valid after the application initializes its protocol without a startup error. A reset before that point should allow the bootloader to return to the previous slot.

Keep the matching USB flash image available for recovery. OTA has not been hardware-tested until a successful update and rollback test have been completed on the actual board.
