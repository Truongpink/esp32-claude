# Hardware profile

## Target board

The product link supplied for this project identifies a Mini ESP32-S3-N16R8 voice-chat board. A matching board identifier appears in upstream XiaoZhi issue [#2206](https://github.com/78/xiaozhi-esp32/issues/2206): **SparkleIoT XH-S3E-AI_V1.0 (ESP32-S3-N16R8)**.

The issue is useful hardware evidence, but its comments contain conflicting reports: the issue author says GPIO 4/5/6 failed on their sample, while a later commenter says the bread-compact-wifi mapping worked on their unit. This points to possible board/revision differences; neither report proves the microphone pins on the user's exact PCB. Confirm the silkscreen/revision before treating this mapping as final.

## Pin evidence

| Function | GPIO / setting | Evidence status |
|---|---:|---|
| ESP target | ESP32-S3 | Product/issue identification |
| Flash / PSRAM | 16 MB / 8 MB | N16R8 product/issue identification |
| OLED | SSD1306 128×64, I²C 0x3C | Product and board-family documentation; verify physical revision |
| OLED SDA / SCL | GPIO 41 / GPIO 42 | XH-S3E-AI PR #2194 board profile; verify revision |
| Speaker I²S DOUT / BCLK / LRCK | GPIO 7 / GPIO 15 / GPIO 16 | Reported working in upstream issue #2206 |
| Speaker amplifier enable | GPIO 17, HIGH | Reported required in upstream issue #2206 |
| Microphone I²S pins | GPIO 4/5/6 candidate | PR #2194 proposes this mapping; issue #2206 has conflicting test results; not verified on the user's unit |
| BOOT button | GPIO 0 candidate | PR #2194 board profile; verify exact PCB |
| Touch button | GPIO 43, active-low | Selected from exact XH-S3E-AI board profile in PR #2194; not physically verified on the user's PCB |
| Volume buttons | GPIO 40 / GPIO 39 candidates | PR #2194 board profile; verify exact PCB |
| Built-in LED | GPIO 48 candidate | PR #2194 board profile; verify exact PCB |

The board-specific upstream PR #2194 configures the speaker output at 16 kHz. This overlay uses 24 kHz so Gemini Live's native 24 kHz PCM can be sent directly to the I²S output without a resampler. The I²S peripheral is configurable, but this 24 kHz speaker path has not yet been verified on the physical board; if playback is distorted or silent, measure/verify the codec path and add resampling rather than silently changing Gemini's PCM rate.

## Important bring-up constraint

The existing `chatai-tpios` overlay was initially copied from upstream `bread-compact-wifi`. Upstream PR [#2194](https://github.com/78/xiaozhi-esp32/pull/2194) proposes GPIO 4/5/6 for the microphone and GPIO 43 for touch, while issue [#2206](https://github.com/78/xiaozhi-esp32/issues/2206) records a board sample where GPIO 4/5/6 did not produce microphone data and later reports where that mapping did work. The overlay now follows the board-specific PR #2194 for GPIO 43 and active-low touch polarity. The microphone mapping remains provisional because issue #2206 reports conflicting results. Treat all candidate pins as unverified until tested on the user's PCB.

Do not call the microphone path working until the actual microphone wiring is established from a schematic or verified hardware test. The confirmed speaker pins alone are not enough to declare two-way voice operation functional.

## Flash and OTA constraints

N16R8 indicates a 16 MB flash target, but firmware must still read the actual flash/partition table and fit the inactive OTA app slot. The upstream v2 16 MB layout uses two 0x3F0000-byte app slots and an 8 MB assets partition. Preserve the device's known-good partition layout during first bring-up; do not repartition merely to add OTA.

See [Web Admin firmware upload OTA design](OTA_WEB_UPLOAD.md).
