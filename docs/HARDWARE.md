# ChatAI TPIOS hardware profile

## Target board identity

- Seller's exact product name: **Kit phát triển Mini ESP32-S3-N16R8 trò chuyện bằng giọng nói AI Xiaozhi**.
- Seller product page: https://thegioimodule.com/kit-phat-trien-mini-esp32-s3-n16r8-tro-chuyen-bang-giong-noi-ai-xiaozhi-co-phat-nhac-tieng-viet-open-source?view=amp
- The product page describes a bundle containing one MINI ESP32-S3-N16R8 board pre-flashed with voice-AI firmware, a speaker cable, and a 0.96-inch OLED. It links directly to https://github.com/78/xiaozhi-esp32.
- Advertised module variant: ESP32-S3-N16R8. The user confirms this unit is 16 MB flash / 8 MB PSRAM.
- Advertised PCB dimensions: 39 × 26 mm.
- **Important identity correction:** the seller page does not identify this board as "XH-S3E-AI V1.0". No matching XH-S3E-AI board directory was found in the pinned Xiaozhi source tree. Do not use that board name as a confirmed hardware identity.
- The seller page does not publish a schematic, PCB revision, bill of materials, microphone part number, audio codec/amplifier part number, or detailed pinout. These remain unknown until a matching schematic, vendor source package, or physical measurement is obtained.

## Evidence levels

| Item | Current evidence | Status |
|---|---|---|
| Product identity | Seller product page | Confirmed as seller's product name only |
| ESP32-S3-N16R8 | Seller listing and user confirmation | Confirmed |
| Flash / PSRAM | User confirms 16 MB / 8 MB | Confirmed for user's unit |
| Speaker cable in bundle | Seller product page | Confirmed as bundle item |
| 0.96-inch OLED in bundle | Seller product page | Confirmed as bundle item |
| OLED controller, resolution, I²C address and GPIO | Current ChatAI firmware configuration / prior project notes, not seller schematic | Firmware-configured; physical verification pending |
| Microphone model and electrical interface | Not identified by seller page or exact matching schematic | Unknown |
| Codec / DAC / amplifier IC | Not identified by seller page or exact matching schematic | Unknown |
| Board PCB revision and exact GPIO routing | No schematic or matching board source located | Unknown |
| BOOT/PTT and volume button electrical mapping | Current ChatAI firmware configuration only | Not electrically verified |

## Current ChatAI firmware pin configuration

These are the pins currently configured in ChatAI source. They are **not a verified pinout for the seller's physical board**.

| Function | GPIO / setting | Evidence and status |
|---|---:|---|
| OLED SDA / SCL | GPIO 41 / GPIO 42 | Current firmware setting; physical board routing unverified |
| OLED driver | SSD1306, configured 128×64, address 0x3C | Current firmware/project setting; seller only confirms a 0.96-inch OLED is included |
| Built-in RGB WS2812 LED | GPIO 48, GRB/RMT | Current firmware setting; physical routing unverified |
| Speaker I²S DOUT / BCLK / WS | GPIO 7 / GPIO 15 / GPIO 16 | Current firmware setting; physical routing unverified |
| Speaker amplifier enable | GPIO 17, HIGH | Current firmware setting; no matching schematic evidence; verify before relying on it |
| BOOT button | GPIO 0, active LOW | Current firmware setting; standard ESP32-S3 BOOT/strapping function, board button wiring not electrically verified |
| Touch / chatbot PTT | GPIO 43, active HIGH | Current firmware setting; physical routing unverified |
| Volume Up | GPIO 40, active LOW | Current firmware setting; physical routing unverified |
| Volume Down | GPIO 39, active LOW | Current firmware setting; physical routing unverified |
| Microphone I²S BCLK / WS / DIN | GPIO 5 / GPIO 4 / GPIO 6 | Current firmware setting; physical routing and microphone interface unverified |
| ESP32-S3 target | esp32s3 | Current build target |

## Xiaozhi upstream source provenance

- Upstream repository linked by the seller: https://github.com/78/xiaozhi-esp32
- Upstream license: MIT. Preserve the upstream license and copyright notices when copying or modifying upstream source.
- ChatAI's current Git submodule is pinned to release v2.6.0, commit `4de00aed740595a8713b01f8332b9b2cf30685bc`, at `firmware/standalone/third_party/xiaozhi-esp32`.
- Upstream repository's current `main` branch is a moving target. For reproducible code comparisons, use the pinned submodule commit unless a deliberate upstream update is approved.
- The pinned source contains a `bread-compact-wifi` board profile whose configuration uses:
  - Microphone I²S WS/SCK/DIN: GPIO 4 / GPIO 5 / GPIO 6.
  - Speaker I²S DOUT/BCLK/LRCK: GPIO 7 / GPIO 15 / GPIO 16.
  - BOOT: GPIO 0.
  - Touch button: GPIO 47.
  - Volume Up/Down: GPIO 40 / GPIO 39.
  - Display SDA/SCL: GPIO 41 / GPIO 42.
  - Built-in LED: GPIO 48.
- This is a **reference profile only**. The seller's Mini ESP32-S3-N16R8 product has not been proven to be the upstream `bread-compact-wifi` board. In particular, the upstream reference uses GPIO 47 for touch while current ChatAI uses GPIO 43 for PTT. Do not change GPIO 43 to GPIO 47 (or vice versa) without confirming the actual PCB.
- The upstream `bread-compact-wifi` config does not establish the seller board's amplifier-enable pin or exact microphone/amp ICs.

## Audio format and current implementation

- ChatAI microphone capture is configured as I²S RX, mono, 16 kHz, 32-bit I²S slots; source converts captured samples to PCM16 for processing.
- ChatAI speaker output is configured as I²S TX, mono, 24 kHz, 32-bit I²S slots; PCM audio is adapted to the configured output path.
- Current I²S DMA settings in `audio_manager.cpp`: six DMA descriptors; microphone frame count 240; speaker frame count 360.
- The current microphone path is configured on I²S controller 1; speaker path on I²S controller 0.
- The actual microphone device type (I²S digital MEMS vs codec-fed I²S) is not established by the seller's product page. Do not infer it from the firmware's I²S RX configuration.
- The actual amplifier IC, enable polarity and GPIO are not established by the seller's product page. GPIO 17 HIGH is only the current firmware setting.
- The upstream Xiaozhi AudioService uses an AudioCodec abstraction, Opus encode/decode, optional resampling, FreeRTOS tasks/queues and an AFE engine on ESP32-S3. This architecture is a software reference, not evidence of the seller board's electrical design.
- ChatAI's AFE input currently has one microphone channel and no synchronized playback-reference channel. Device AEC is therefore not operational. Noise suppression and AGC are disabled. WakeNet is unavailable without the required ESP-SR model partition/assets; the current configuration uses WebRTC VAD fallback as documented in `XIAOZHI_INTEGRATION.md`.

## BOOT/PTT investigation constraints

- GPIO0 is the ESP32-S3 strapping/BOOT pin. A normal running application can read it as an input, but the physical button wiring and board behavior must be confirmed.
- Current ChatAI config treats BOOT as GPIO0 active LOW and PTT as GPIO43 active HIGH.
- The upstream `bread-compact-wifi` reference assigns touch to GPIO47, not GPIO43. This difference is a warning against treating that reference board as the user's exact board.
- A reset when pressing BOOT/PTT must be diagnosed using reset reason, panic/backtrace and button/audio diagnostic markers. Do not assume it is a pin conflict or stack overflow without evidence.
- Before changing GPIO mappings, obtain the exact board schematic or perform safe continuity/voltage measurements on the physical board.

## Flash and partition layout

Standalone now targets the 16 MB dual-slot OTA layout used by the earlier Xiaozhi-derived ChatAI firmware. It preserves NVS at 0x9000, otadata at 0xD000, ota_0 at 0x20000, and ota_1 immediately after ota_0; each app slot is 0x3F0000 bytes. The user confirmed the N16R8 board variant. The installed physical partition table has not been read back independently, so the first application-only OTA should be attempted only after confirming the running firmware reports the expected OTA target and available slot size.

## Source references checked

- Seller product listing: https://thegioimodule.com/kit-phat-trien-mini-esp32-s3-n16r8-tro-chuyen-bang-giong-noi-ai-xiaozhi-co-phat-nhac-tieng-viet-open-source?view=amp
- Upstream repository: https://github.com/78/xiaozhi-esp32
- Pinned upstream board reference: `main/boards/bread-compact-wifi/config.h` at commit `4de00aed740595a8713b01f8332b9b2cf30685bc`
- Pinned upstream AudioService: `main/audio/audio_service.cc`
- Pinned upstream AFE engine: `main/audio/engines/afe_audio_engine.cc`
- ChatAI current I²S implementation: `firmware/standalone/main/audio_manager.cpp`
- ChatAI current button implementation: `firmware/standalone/main/button_manager.cpp`
- ChatAI integration notes: `firmware/standalone/XIAOZHI_INTEGRATION.md`

Last reviewed: 2026-11-23.
