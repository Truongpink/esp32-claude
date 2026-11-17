# Hardware profile

## Target board

The product link supplied for this project identifies a Mini ESP32-S3-N16R8 voice-chat board. A matching board identifier appears in upstream XiaoZhi issue [#2206](https://github.com/78/xiaozhi-esp32/issues/2206): **SparkleIoT XH-S3E-AI_V1.0 (ESP32-S3-N16R8)**.

The issue is useful hardware evidence, but the product listing alone does not prove that every seller/revision uses the exact same PCB. Confirm the silkscreen/revision before treating this mapping as final.

## Pin evidence

| Function | GPIO / setting | Evidence status |
|---|---:|---|
| ESP target | ESP32-S3 | Product/issue identification |
| Flash / PSRAM | 16 MB / 8 MB | N16R8 product/issue identification |
| OLED | SSD1306 128×64, I²C 0x3C | Product and board-family documentation; verify physical revision |
| OLED SDA / SCL | GPIO 41 / GPIO 42 | Candidate from board-family source; verify revision |
| Speaker I²S DOUT / BCLK / LRCK | GPIO 7 / GPIO 15 / GPIO 16 | Reported working in upstream issue #2206 |
| Speaker amplifier enable | GPIO 17, HIGH | Reported required in upstream issue #2206 |
| Microphone I²S pins | UNKNOWN | Not confirmed; issue #2206 reports GPIO 4/5/6 did not work |
| BOOT / touch / volume buttons | UNKNOWN | Do not assume the bread-compact-wifi mapping |
| Built-in LED | UNKNOWN | Do not assume GPIO 48 |

## Important bring-up constraint

The existing `chatai-tpios` overlay was initially copied from upstream `bread-compact-wifi`. Its GPIO 4/5/6 microphone assignment, GPIO 47 touch button and GPIO 40/39 volume buttons are **not confirmed for XH-S3E-AI_V1.0**. In particular, upstream issue #2206 reports that GPIO 4/5/6 did not produce microphone data on the reported board.

Do not call the microphone path working until the actual microphone wiring is established from a schematic or verified hardware test. The confirmed speaker pins alone are not enough to declare two-way voice operation functional.

## Flash and OTA constraints

N16R8 indicates a 16 MB flash target, but firmware must still read the actual flash/partition table and fit the inactive OTA app slot. The upstream v2 16 MB layout uses two 0x3F0000-byte app slots and an 8 MB assets partition. Preserve the device's known-good partition layout during first bring-up; do not repartition merely to add OTA.

See [Web Admin firmware upload OTA design](OTA_WEB_UPLOAD.md).
