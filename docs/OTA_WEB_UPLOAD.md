# Web Admin local firmware upload (OTA) — design

## User workflow

1. Open the device's local Web Admin from a phone.
2. Select an application firmware `.bin` file.
3. Upload it over the device's local HTTP server.
4. Show received bytes/progress and a clear success or failure result.
5. Reboot only after the inactive slot has been completely written and validated.

The first USB/serial installation may require a merged image (bootloader, partition table, app and any required data partitions). The Web Admin OTA uploader accepts an **application OTA image**, not a merged flash image.

## Device-side update sequence

Use ESP-IDF's native OTA APIs and the inactive application partition:

1. Reject missing/empty uploads and enforce the inactive partition's maximum size before writing.
2. Obtain the next update partition with `esp_ota_get_next_update_partition(nullptr)`.
3. Start with `esp_ota_begin`, stream request chunks to `esp_ota_write`, then finish with `esp_ota_end`.
4. Treat any short write, disconnect, size mismatch or image validation error as failure; abort the OTA handle and leave the current boot partition unchanged.
5. Only after successful validation call `esp_ota_set_boot_partition` for the new slot.
6. Respond with success and reboot. Do not reboot while the upload is incomplete.

Do not buffer the entire firmware in RAM. Stream bounded chunks from the HTTP request body to flash.

## Rollback

Enable ESP-IDF application rollback in the bootloader configuration. On first boot of the new image, run a short self-test covering NVS/config load, Web Admin startup, board/display initialization and required system services. Call `esp_ota_mark_app_valid_cancel_rollback()` only after the self-test passes. If startup fails or the image never marks itself valid, let the bootloader revert to the previous valid OTA slot.

The first bring-up release should keep the original known-good firmware available in its existing slot. Never erase or overwrite the running app partition.

## Partition constraints

The pinned upstream `sdkconfig.defaults` selects `partitions/v2/16m.csv` with a 16 MB flash setting. That table contains `ota_0` at offset 0x20000 with size 0x3F0000, `ota_1` with size 0x3F0000, and an `assets` partition at 0x800000 with size 8 MB.

This is the upstream default, not proof of the active generated `sdkconfig` or the layout already installed on the user's board. A previously generated `sdkconfig` may override defaults. Before enabling OTA, inspect the actual build partition CSV and the currently installed device layout. The OTA app image must fit in one app slot. N16R8 is not by itself proof that a particular physical board is running this exact layout.

## Minimal Web API

- `POST /api/ota`: raw multipart file upload; stream to inactive partition.
- `GET /api/status`: report current firmware version, active partition and OTA capability; never return secrets.
- The upload response reports accepted bytes and final validation result. The browser may show progress using upload events, but the device remains responsible for validating the image.

The current Web Admin has no PIN or login and uses plain HTTP. Anyone on the trusted LAN can change settings. If OTA is added under this no-login requirement, anyone on that LAN will also be able to replace firmware. Do not expose OTA upload to an untrusted network; local LAN access is the intended first-release scope.

## Not implemented yet

This document defines the implementation contract. The upload handler, rollback configuration and UI are not yet implemented or tested. No build or flash workflow has been run.
