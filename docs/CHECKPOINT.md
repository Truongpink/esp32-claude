# ChatAI TPIOS — project checkpoint

**Checkpoint date:** 2026-11-17 (ICT)  
**Repository:** `Truongpink/esp32-claude`  
**Working branch:** `main` only  
**Repository HEAD before this checkpoint update:** `7b1f803973f7a459017c0a4bf98d5d7b9b7ef585` — `Fix semantic version validation for OTA release`

## Hardware bring-up — user-reported working

The user has installed the firmware on the ESP32-S3 successfully and confirmed:

- Firmware installation completed and the device starts sufficiently to continue configuration.
- Wi-Fi connection is working.
- Local Web Admin is reachable from the LAN at the device's current address; the user confirmed `http://192.168.1.110` during the setup session. This address is DHCP-assigned and may change.
- The user can open the Web Admin page.

These are user-reported hardware results. Do not treat them as proof that every hardware feature or every API endpoint is fully validated.

## Current feature status

### Confirmed by user on hardware
- [x] Firmware can be installed.
- [x] Device connects to Wi-Fi.
- [x] Web Admin can be opened over the LAN.

### Implemented in source; hardware verification still pending
- ChatAI TPIOS board overlay for ESP32-S3.
- Gemini Live WebSocket protocol integration and PCM audio paths (16 kHz mono PCM16 microphone input, 24 kHz mono PCM16 speaker output).
- Web Admin configuration/status endpoints and NVS-backed Gemini settings, speaker volume, assistant name, selected Live voice, response language, microphone gain, and custom wake phrase.
- English MultiNet7 wake-word model enabled for ESP32-S3; default phrase is “Hey ChatAI”, with runtime phrase replacement and NVS persistence.
- Both voice wake detection and the existing BOOT short-press activation are retained; BOOT long-press still enters Wi-Fi provisioning.
- Gemini Live now defaults to realtime listening mode so the microphone path remains active during assistant playback for continuous turn-taking and barge-in; hardware behavior still needs verification.
- Web Admin includes 30 Gemini Live prebuilt voice choices, language selection, microphone software gain, restart, and factory-reset controls.
- OTA upload progress is also sent to the OLED status line while the image is written.
- Application-only OTA upload to the inactive OTA partition, with image validation and boot-slot selection.
- ESP-IDF rollback support, with the new image marked valid after application/protocol initialization completes without a startup error.
- Manual-only GitHub Actions workflow now builds and publishes only the application OTA image; merged USB image generation and USB artifact upload have been removed.
- Release version is read from `firmware/VERSION` (currently `0.1.2`) and injected into upstream `PROJECT_VER`, so the ESP application descriptor and OTA artifact use the same version.

### Not yet confirmed / next tests
- The user previously reported a TTS-like interaction. The source now uses Gemini Live realtime listening mode and keeps the microphone path active during assistant playback; verify natural voice-to-voice turn-taking and interruption on hardware.
- Verify “Hey ChatAI” recognition, user-defined English wake phrases, BOOT activation, and wake-word interruption during an active session.
- Verify the 30-voice selector and language setting against the selected Gemini Live model.
- Verify microphone gain adjustment (0–20 dB) does not introduce unacceptable clipping or noise.
- OTA has **not** yet been tested on hardware. The OTA file is reserved for testing future firmware versions after the current USB-installed firmware is known-good.
- Verify OTA success, reboot into the new slot, persistence of Wi-Fi/Gemini settings, and rollback/recovery on the actual board.
- Verify OLED, microphone capture quality, speaker output, buttons, and volume independently.

## USB-FLASH vs OTA

- **USB-FLASH merged image:** for initial installation or recovery/re-flashing over USB.
- **OTA application-only `.bin`:** for updating a firmware already installed on the device through Web Admin; do not upload the merged USB image to the OTA endpoint.
- Intended version flow: install the baseline with USB-FLASH, then test subsequent releases (for example v0.1.2, v0.1.3) using OTA.
- Keep the matching USB-FLASH image as a recovery option.
- Do not claim OTA is working until an actual hardware update and recovery test succeeds.

## Next OTA release preparation — v0.1.2

- Next release artifact name: `ChatAI-TPIOS-v0.1.2-OTA.bin`.
- `.github/workflows/build-firmware.yml` remains `workflow_dispatch` only and publishes exactly one OTA artifact.
- The workflow no longer runs `idf.py merge-bin` and no longer creates/uploads a merged USB-FLASH image.
- Essential preparation remains: pinned upstream checkout, ChatAI overlay, Wi-Fi portal asset patch, required CMake reconfigure passes, and ESP-IDF application build.
- `firmware/VERSION` is the single release-version source. `scripts/apply_upstream_patches.py` validates it and applies it to upstream `PROJECT_VER`.
- The semantic-version validation expression was checked after correction.
- Source/workflow changes only: no GitHub Action, firmware build, OTA upload, or hardware test has been run for v0.1.2. The workflow still requires a manual run, followed by an actual OTA test on the board.

## Remaining development and verification

1. Verify the new continuous Gemini Live session and barge-in behavior on the actual board.
2. Add a real Gemini connectivity test that does not start a user conversation.
3. Verify Wi-Fi provisioning, restart, and factory reset from the local Web Admin.
4. Verify OTA upload progress on OLED, settings persistence, boot-slot switch, and rollback on hardware.
5. For subsequent releases, use OTA rather than USB once the installed baseline and OTA compatibility are verified. Keep Actions manual-only and publish one versioned OTA application binary.
## Recent compile fix

The previous Actions compile failure was caused by calling `AudioCodec::SetOutputVolume()` while only the forward declaration was visible. The source was fixed by including `audio_codec.h` in `firmware/board/chatai-tpios/web_admin_server.cc`.

Fix commit: `5fd0106b48815a40fa93ed2077ec6a8f47c13cf9`.

The repository HEAD and fixed source file were re-fetched from `main`. Do not claim a post-fix Actions build passed unless a newer run is inspected.

## Latest source-only feature batch

- Committed to `main` through `14c2bc4` (wake phrase, continuous listening, Web Admin voice/language/mic gain, Wi-Fi reconfiguration, restart/factory reset, and OLED OTA progress).
- The HTML JavaScript syntax, Web Admin element references, config JSON, API route count, and pinned-upstream patch anchors were checked.
- No GitHub Actions, firmware build, OTA upload, or hardware operation was run. These changes are not yet compile-verified.
- Keep `firmware/VERSION` at `0.1.2` until the next build is explicitly authorized; the next build should use `0.1.3`.

## Project implementation notes

- Upstream XiaoZhi source is pinned to commit `4de00aed740595a8713b01f8332b9b2cf30685bc`.
- Web Admin setup AP uses `192.168.4.1` while AP provisioning is active. After joining the home Wi-Fi, use the device's router-assigned LAN IP instead.
- OTA endpoint accepts an application-only binary as raw `application/octet-stream`, streams it to the inactive OTA slot, validates it, selects it, and restarts.
- The rollback validity check is based on application/protocol initialization; it does not guarantee that a later Gemini cloud session will connect successfully.
- The current Web Admin uses plain HTTP and has no PIN/login. Keep it on a trusted local network; do not expose it to the public Internet.

## Rules for continuing work

- Work on `main` only; never create or switch branches.
- Before every edit, inspect current branch HEAD, repository state, and the exact relevant source files.
- Make minimal targeted changes; preserve working Wi-Fi, Web Admin, configuration, and audio functionality.
- Do not modify unrelated features.
- GitHub Actions is manual-only (`workflow_dispatch`). Never add push/PR triggers.
- Never run Actions, builds, flashing, or hardware operations without the user's explicit permission.
- Report hardware behavior as verified only when the user or an actual test confirms it.
