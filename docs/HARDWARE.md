# ChatAI TPIOS hardware profile

## Target

- Board family: ESP32-S3 voice-chat board, reported as XH-S3E-AI V1.0.
- Memory variant reported by the product description: N16R8 (16 MB flash / 8 MB PSRAM). Confirm the actual module marking and flash configuration on the physical board before relying on these values.
- Display: SSD1306, 128×64, I²C address 0x3C.

## Current firmware pin configuration

| Function | GPIO / setting | Status |
|---|---:|---|
| OLED SDA / SCL | GPIO 41 / GPIO 42 | Configured; physical revision still needs confirmation |
| Speaker I²S DOUT / BCLK / WS | GPIO 7 / GPIO 15 / GPIO 16 | Reported board mapping; verify on the exact PCB |
| Speaker amplifier enable | GPIO 17, HIGH | Reported board mapping; verify on the exact PCB |
| Microphone I²S BCLK / WS / DIN | GPIO 5 / GPIO 4 / GPIO 6 | Provisional; not verified on the user's unit |
| ESP32-S3 target | esp32s3 | Build target |

These are configuration/evidence notes, not a substitute for the exact board schematic or an electrical test. Do not describe the microphone path as working until capture has been verified on the actual board.

## Audio format

- Microphone: mono PCM16, 16 kHz.
- Speaker: mono PCM16, 24 kHz.
- Speaker amplifier enable: GPIO 17 HIGH (subject to physical confirmation).

## Flash and partition layout

The Standalone defaults currently select ESP-IDF's 1.5 MB single-app partition layout. The physical flash size and installed partition table have not been read back from the user's board. OTA slots are not present in this layout and OTA is not implemented in the current Standalone firmware.

Do not change flash size or partition layout without explicit confirmation from the actual hardware.
