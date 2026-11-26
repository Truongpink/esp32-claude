# ECC Shin — project continuation guide (ChatAI TPIOS + Xiaozhi)

> **Read this file first when resuming the project.**  
> Last recorded state: 2026-11-26  
> Repository: `Truongpink/esp32-claude`  
> Branch: `main` only  
> Last firmware source commit before documentation capture: `c251368ac66099c6846379440a6eeafb4c442c37`  
> Successful workflow: #97, Build #80, run ID `45809252292`.

## 1. Current state at a glance

- The project is ChatAI TPIOS standalone firmware with Xiaozhi v2.6.0 integrated as the only active voice client.
- Xiaozhi source is pinned as a Git submodule at `firmware/standalone/third_party/xiaozhi-esp32`, release `v2.6.0`, commit `4de00aed740595a8713b01f8332b9b2cf30685bc`.
- ChatAI still owns Wi-Fi/NetworkManager, Web Admin, AudioManager/I2S, OLED, buttons, diagnostics, NVS, partition layout and manual OTA.
- The Xiaozhi audio path adapts to ChatAI's existing I2S through the AudioCodec bridge. Do not add a second I2S driver or second Wi-Fi owner.
- Gemini source/settings may remain in the repository, but Gemini is not active in the current voice session path.
- Bluetooth/BLE are disabled.
- GitHub Actions run #97 built successfully on commit `c251368...`; Build #80 image size was 3,378,016 bytes versus a 4,128,768-byte OTA app slot. Artifact upload succeeded.
- User confirmed firmware installation and that the device boots/runs “tạm ổn”; further device testing is pending. Do not describe the system as fully stable yet.

## 2. Mandatory reading order

1. `docs/ECC_SHIN_PROJECT_CONTINUATION.md` — this file.
2. `docs/ECC_SHIN_XIAOZHI_MIGRATION_HISTORY.md` — complete migration timeline, decisions, build failures/fixes and test plan.
3. `firmware/standalone/XIAOZHI_INTEGRATION.md` — detailed current integration contract, codec/audio/protocol behavior and AFE limits.
4. `docs/HARDWARE.md` — board provenance, configured GPIO and evidence limitations.
5. Relevant source files listed in §3.

**Do not use the old Gemini-only statements in `README.md` or `docs/STANDALONE_MIGRATION.md` as the current architecture.** Those documents are historical/stale relative to the Xiaozhi-integrated source. See the migration history's documentation-conflict section.

## 3. Source map and trace paths

### Startup / application
- `firmware/standalone/main/app_main.cpp`
- `firmware/standalone/main/chatai_xiaozhi_client.*`
- `firmware/standalone/main/chatai_xiaozhi_application.*`
- `firmware/standalone/main/chatai_xiaozhi_protocol.*`
- `firmware/standalone/main/chatai_xiaozhi_board.cpp`

Trace initialization order from `app_main`: NVS/platform → ChatAI NetworkManager/Wi-Fi → display/audio → Xiaozhi client/application → button and runtime operation. Confirm current source before assuming every step or callback remains unchanged.

### Audio ownership chain
`AudioManager (I2S)` → `ChatAIAudioCodecBridge` → `ChatAIXiaozhiAudioCodec` / Opus adapter → Xiaozhi AudioService/AFE/runtime → Xiaozhi session transport.

Key files:
- `audio_manager.*`
- `audio_codec.*`
- `chatai_audio_codec_bridge.*`
- `chatai_xiaozhi_audio_codec.*`
- `chatai_xiaozhi_opus_codec.*` (if present in current tree)
- `audio_service.cc`
- `afe_audio_engine.cc`
- `chatai_xiaozhi_client.*`

Do not change sample rates, frame durations, queue limits, resampling, task lifecycle or microphone ownership without tracing the full chain and protocol-negotiated format.

### Network ownership chain
ChatAI `NetworkManager` owns Wi-Fi and provisioning. Xiaozhi receives network-state callbacks and waits for connectivity; it must not initialize a competing Wi-Fi stack.

Key files:
- `network_manager.*`
- `chatai_xiaozhi_board.cpp`
- `chatai_xiaozhi_client.*`
- `chatai_xiaozhi_protocol.*`
- `web_admin.*`

Review activation, OTA config discovery, SNTP/TLS, MQTT/UDP, WebSocket fallback, reconnect, setup-AP restoration and session cancellation together when changing connectivity.

### Web Admin / diagnostics / OTA
- `web_admin.*`, `diagnostic_manager.*`, `display_manager.*`, `button_manager.*`, `status_led_manager.*`
- `.github/workflows/build-standalone.yml`
- `firmware/standalone/main/CMakeLists.txt`
- `firmware/standalone/sdkconfig.defaults`
- `firmware/standalone/partitions/16m.csv`

Preserve existing routes and route-registration capacity. The earlier project state recorded 28 registered Web Admin routes and configured capacity 28; verify the current exact source before adding routes because no spare capacity was recorded.

## 4. Hard engineering rules

### Repository
- Work on `main` only. No branches, forks or new repositories.
- Before every edit: fetch latest main HEAD, inspect repository status/tree, fetch exact target file content and blob SHA.
- Make minimal, evidence-based changes.
- Do not modify Xiaozhi submodule source or pinned submodule pointer unless separately approved.
- Preserve upstream MIT license and copyright notices.
- After edits, inspect diff and fetch the committed file/HEAD to verify.

### Permissions
- Do not run GitHub Actions, Codemagic, local build, flash, OTA or deployment unless the user explicitly authorizes that specific operation.
- Permission to inspect/fix source does not imply permission to trigger a build.
- Never claim compile/runtime verification unless the relevant action was actually performed.
- User has authorized commits to `main` for in-scope source work; that does not authorize build/flash/OTA.
- When user asks only to inspect logs or source, do not trigger Actions.

### Architecture invariants
- Keep ChatAI NetworkManager as the only Wi-Fi owner.
- Keep ChatAI AudioManager as the only I2S channel/driver owner.
- Keep Web Admin, NVS/configuration, OLED, buttons, status LED, diagnostics, current partition layout and manual OTA.
- Keep Bluetooth/BLE disabled; do not add NimBLE scanning.
- Do not enable Gemini alongside Xiaozhi or add audio arbitration unless explicitly redesigned and approved.
- Do not import the upstream `bread-compact-wifi` pinout as if it were proven to match the user's physical board.
- Do not claim WakeNet, device AEC or NS is available without model assets and/or a validated reference path.
- Do not erase NVS or repartition flash as a convenience step.

## 5. Hardware facts and cautions

User's module variant is ESP32-S3-N16R8 (16 MB flash / 8 MB PSRAM). Current firmware-configured pins are:
- OLED SDA/SCL: 41/42.
- Mic I2S BCLK/WS/DIN: 5/4/6.
- Speaker I2S DOUT/BCLK/WS: 7/15/16.
- Amp enable: 17 HIGH.
- BOOT: 0 active LOW.
- Touch/PTT: 43 active HIGH.
- Volume Up/Down: 40/39 active LOW.
- RGB LED: 48.

These are firmware settings, not a fully verified electrical schematic. Seller listing does not confirm the exact board PCB identity as “XH-S3E-AI V1.0”. The upstream `bread-compact-wifi` profile uses touch GPIO47, whereas ChatAI currently uses GPIO43. Never change pins based only on that upstream reference. See `docs/HARDWARE.md`.

## 6. Known feature limitations and technical cautions

- Current PCM bridge: mono PCM16, 16 kHz microphone input / 24 kHz speaker output.
- Opus uplink uses 16 kHz mono / 60 ms frames (960 samples).
- Current downlink accepts 24 kHz mono / 60 ms only; other formats should be rejected until a correct resampler is integrated.
- Audio runtime has bounded four-packet queues; oldest packet is dropped on full queue to constrain latency.
- VAD fallback is WebRTC when no VAD model is supplied; current VAD settings and diagnostics are documented in `XIAOZHI_INTEGRATION.md`.
- WakeNet unavailable without model partition/assets.
- Device AEC unavailable without synchronized playback reference.
- Noise suppression disabled without NSNet model.
- Software microphone gain -6..+6 dB is PCM gain before AFE, not hardware preamp gain.
- MQTT+UDP is used when returned by OTA config; otherwise WebSocket. Preserve the upstream OTA transport precedence and negotiated audio format.
- Protocol credentials and activation challenges must not be logged.

## 7. Next test sequence (no code changes implied)

Record test date, firmware build number, reset reason, logs and outcome for each item.

1. **Baseline boot:** cold boot and warm reboot; check reset reason, firmware build number, Wi-Fi connection, OLED, LED and Web Admin.
2. **Idle stability:** leave device idle for 30–60 minutes; monitor free/minimum heap, Wi-Fi stability and spontaneous resets.
3. **One-shot BOOT:** run 10 consecutive manual turns; check listen start, silence stop, server response, TTS completion and return to idle.
4. **PTT:** test press/hold/release and confirm no collision with BOOT one-shot flow.
5. **Audio quality:** test quiet/normal/loud speech, clipping, missed speech, recognition and speaker distortion; record `MIC_LEVEL`, `MIC_VAD`, uplink counters and latency.
6. **Long response:** test long TTS and multiple consecutive responses; ensure microphone is paused/resumed correctly.
7. **Network recovery:** router restart, temporary Wi-Fi loss, IP change and reconnect; verify Web Admin and Xiaozhi session recover.
8. **Activation/protocol:** test activation state and reconnect; identify whether OTA config selects MQTT+UDP or WebSocket from logs without exposing secrets.
9. **Extended stability:** run several hours with repeated sessions; compare heap minima and reboot/crash markers over time.
10. **Manual OTA:** only when separately authorized. Check target partition/size, boot confirmation, rollback and retained NVS/Wi-Fi settings.

Do not infer a root cause from “Load failed”, reset reason or one log line alone. Capture the first failure marker, timestamp, reset reason, backtrace and surrounding subsystem logs.

## 8. How to respond to a new failure

1. Ask for the smallest useful evidence: exact error/log and what action caused it. Avoid asking for a screenshot if text log is available.
2. Fetch current GitHub main HEAD and latest workflow run list.
3. If CI failed, fetch the failed job and full log. Identify the first real compiler/configuration error and distinguish it from downstream failures.
4. Inspect exact current source, CMake dependency declarations, SDK config and submodule revision.
5. Propose/fix only the demonstrated cause; do not globally suppress warnings or disable features to force success.
6. Re-fetch and verify committed source.
7. State clearly what is source-verified, CI-verified, user-tested and still unverified.
8. Do not rerun Actions/build unless explicitly authorized.

## 9. GitHub baseline references

- Repository: https://github.com/Truongpink/esp32-claude
- Last firmware source commit: https://github.com/Truongpink/esp32-claude/commit/c251368ac66099c6846379440a6eeafb4c442c37
- Successful workflow #97: https://github.com/Truongpink/esp32-claude/actions/runs/45809252292
- Pinned Xiaozhi upstream: https://github.com/78/xiaozhi-esp32/tree/4de00aed740595a8713b01f8332b9b2cf30685bc

## 10. Retrieval phrases

The user can say:
- **“Mở hồ sơ Xiaozhi”** — read the full migration history.
- **“Mở lịch sử chuyển đổi ChatAI TPIOS sang Xiaozhi”** — inspect research, decisions, commits and build fixes.
- **“ECC Shin tiếp tục”** — resume from the current repository state and this guide.
- **“Khôi phục kiến trúc ChatAI TPIOS”** — compare the earlier Gemini-oriented ChatAI design with the current Xiaozhi-integrated design; do not revert anything without a separate explicit decision.

## 11. Short Memory / Personalization handoff

Use this compact summary when transferring the project context into ChatGPT Memory/Personalization:

> **ECC Shin / ChatAI TPIOS ESP32-S3:** Repository `Truongpink/esp32-claude`, work on `main` only. Current firmware integrates Xiaozhi v2.6.0 (submodule commit `4de00aed740595a8713b01f8332b9b2cf30685bc`) as the sole active voice client. ChatAI retains ownership of NetworkManager/Wi-Fi, Web Admin, AudioManager/I2S, OLED, buttons, diagnostics, NVS and manual dual-slot OTA; Xiaozhi uses ChatAI's AudioCodec bridge, no second Wi-Fi or I2S driver. Gemini is not active in current voice path. BLE disabled. ESP32-S3 N16R8, 16 MB flash/8 MB PSRAM; configured pins are in `docs/HARDWARE.md` and are not all physically verified. Build #80 succeeded in GitHub Actions run #97 on source commit `c251368ac66099c6846379440a6eeafb4c442c37`; user confirmed install and device runs “tạm ổn”, further stability testing pending. Read `docs/ECC_SHIN_PROJECT_CONTINUATION.md` then `docs/ECC_SHIN_XIAOZHI_MIGRATION_HISTORY.md`. Before edits inspect latest main HEAD and exact file SHA; minimal evidence-based changes; no branches; do not edit Xiaozhi submodule/pointer; do not run Actions/build/flash/OTA without separate explicit permission. Preserve Wi-Fi, Web Admin, AudioManager, OLED, NVS, partition and OTA; do not enable BLE or Gemini or claim WakeNet/AEC/NS without proof.
