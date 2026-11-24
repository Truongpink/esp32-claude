# ChatAI TPIOS — Project Memory

## Identity
- Name: ChatAI TPIOS
- Repository: https://github.com/Truongpink/esp32-claude
- Main branch: `main` only
- Target: ESP32-S3
- Firmware project: `firmware/standalone`
- Build system documented by repository: ESP-IDF 6.1

## Mission and scope
Independent ESP32-S3 firmware with native networking, audio I/O, OLED, local Web Admin, persistent settings and Gemini Live integration as described by current README. Preserve working behavior and avoid unrelated refactors.

## Mandatory working rules
See root `AGENTS.md` and `../CORE/ECC-SHIN-CORE.md`. ASK before changes; no new branches; no Actions/build/flash/OTA/deploy without separate permission. Web Admin UI is protected baseline.

## Evidence status
- VERIFIED (repository docs): README describes direct Gemini Live over WSS/TLS and local Web Admin.
- VERIFIED (repository docs): firmware project path is `firmware/standalone`; target is ESP32-S3.
- VERIFIED (repository rules): branch `main` only and Web Admin UI protection.
- NOT TESTED: physical board behavior, full Gemini conversation and independently read-back partition table.
- Treat all device behavior not directly tested as NOT TESTED.

## Memory maintenance
Update this file only when project identity/scope changes. Put architecture in ARCHITECTURE, durable choices in TECHNICAL-DECISIONS, current progress in PROGRESS, and unresolved blockers in ISSUES-HANDOFF.
