# ChatAI TPIOS — standalone ESP-IDF project

This directory is the new independent firmware project. It is deliberately separate
from the existing overlay-based firmware so the known working release path remains
untouched while components are migrated and verified.

## Requirements

- ESP-IDF 6.1
- ESP32-S3 target

## Configure

From the repository root:

```sh
idf.py -C firmware/standalone set-target esp32s3
idf.py -C firmware/standalone reconfigure
```

Build and flash are intentionally not part of this migration step. Do not flash this
scaffold: it does not yet contain the board drivers, Gemini Live client, Web Admin,
or OTA implementation.

## Migration rules

- This project must not fetch, include, or link XiaoZhi source.
- Use native ESP-IDF services and ChatAI-owned modules.
- Keep the existing firmware overlay intact until the standalone replacement has
  passed compile, device, audio, network, and OTA verification.
- Do not define a flash size or partition table until the physical board's current
  flash and partition layout have been read and confirmed.
