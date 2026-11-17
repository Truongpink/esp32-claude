# Hardware profile

The current candidate profile is derived from the upstream `bread-compact-wifi` implementation in XiaoZhi ESP32 v2.6.0. A separate `chatai-tpios` board identity is used so OTA identity does not collide with the upstream board.

| Function | GPIO / setting |
|---|---|
| ESP target | ESP32-S3 |
| OLED | SSD1306 128×64, I²C 0x3C |
| OLED SDA / SCL | GPIO 41 / GPIO 42 |
| Microphone I²S WS / SCK / DIN | GPIO 4 / GPIO 5 / GPIO 6 |
| Speaker I²S DOUT / BCLK / LRCK | GPIO 7 / GPIO 15 / GPIO 16 |
| BOOT button | GPIO 0 |
| Touch button | GPIO 47 |
| Volume up / down | GPIO 40 / GPIO 39 |
| Built-in LED | GPIO 48 |
| Audio input / output | 16 kHz / 24 kHz |

These values are a source-derived candidate, not proof that every hardware revision has identical wiring. The actual board revision and flash/PSRAM capacity must be confirmed before final partition or OTA decisions.

The source overlay intentionally does not change partition tables or flash-size settings.
