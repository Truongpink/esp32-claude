# ChatAI TPIOS — Architecture Memory

## Repository-documented architecture
- Target: ESP32-S3; standalone ESP-IDF project at `firmware/standalone`.
- README describes firmware-owned Wi-Fi, audio I/O, OLED, local Web Admin, NVS settings and direct Gemini Live WSS/TLS connection.
- README describes mono PCM16 microphone input at 16 kHz and speaker output at 24 kHz.
- Web Admin is local and handles Gemini API key entry/replacement/deletion.
- Display documented as SSD1306 128×64 at I²C address 0x3C.

## Security and deployment boundaries
- README states local Web Admin uses HTTP and NVS encryption is not enabled; restrict access to trusted local networks.
- README documents application-only OTA against a 16 MB dual-slot layout. The physical board's installed partition table has not been independently read back.
- Do not expose Web Admin publicly or log API keys.

## Verification limits
The above is repository documentation evidence, not proof of runtime behavior. Confirm current source before modifying architecture. Hardware pin mapping, microphone/speaker behavior, complete Gemini Live conversation and installed partition layout remain NOT TESTED unless later evidence is recorded.
