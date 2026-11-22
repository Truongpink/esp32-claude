# ChatAI TPIOS hardware profile

## Target

- Board family: ESP32-S3 voice-chat board, reported as XH-S3E-AI V1.0.
- Memory variant: N16R8 (16 MB flash / 8 MB PSRAM), confirmed by the user.
- Display: SSD1306, 128×64, I²C address 0x3C.

## Current firmware pin configuration

| Function | GPIO / setting | Status |
|---|---:|---|
| OLED SDA / SCL | GPIO 41 / GPIO 42 | Configured; physical revision still needs confirmation |
| Built-in RGB WS2812 LED | GPIO 48, GRB/RMT | Board support profile; firmware status LED uses this pin |
| Speaker I²S DOUT / BCLK / WS | GPIO 7 / GPIO 15 / GPIO 16 | Reported board mapping; verify on the exact PCB |
| Speaker amplifier enable | GPIO 17, HIGH | Reported board mapping; verify on the exact PCB |
| BOOT button | GPIO 0, active LOW | XH-S3E-AI V1.0 board profile |
| Touch / chatbot PTT | GPIO 43, active HIGH | XH-S3E-AI V1.0 board profile |
| Volume Up | GPIO 40, active LOW | XH-S3E-AI V1.0 board profile |
| Volume Down | GPIO 39, active LOW | XH-S3E-AI V1.0 board profile |
| Microphone I²S BCLK / WS / DIN | GPIO 5 / GPIO 4 / GPIO 6 | Board profile mapping; physical signal still needs verification |
| ESP32-S3 target | esp32s3 | Build target |

Button assignments follow the XH-S3E-AI V1.0 board-support profile from the Xiaozhi project; they have not been electrically probed on the user's physical unit. These are configuration/evidence notes, not a substitute for the exact board schematic or an electrical test. Do not describe the microphone path as working until capture has been verified on the actual board.

## Audio format

- Microphone: mono PCM16, 16 kHz.
- Speaker: mono PCM16, 24 kHz.
- Speaker amplifier enable: GPIO 17 HIGH (subject to physical confirmation).

## Flash and partition layout

Standalone now targets the 16 MB dual-slot OTA layout used by the earlier Xiaozhi-derived ChatAI firmware. It preserves NVS at 0x9000, otadata at 0xD000, ota_0 at 0x20000, and ota_1 immediately after ota_0; each app slot is 0x3F0000 bytes. The user confirmed the N16R8 board variant. The installed physical partition table has not been read back independently, so the first application-only OTA should be attempted only after confirming the running firmware reports the expected OTA target and available slot size.
