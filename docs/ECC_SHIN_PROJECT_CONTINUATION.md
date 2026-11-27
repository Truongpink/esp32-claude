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

### Wi-Fi architecture standard (mandatory)

**Owner:** ChatAI `NetworkManager` is the sole Wi-Fi/radio owner. Xiaozhi/Gemini/AI clients consume network-state callbacks; they must never initialize a competing Wi-Fi stack or control STA/SoftAP lifecycle.

**Required behavior:**
- On every boot, start the setup SoftAP and STA together in APSTA mode. The setup SSID format is `ChatAI-TPIOS-XXXXXX`; its current setup address is `192.168.50.1`. Start the local Web Admin so recovery does not depend on router or Internet availability.
- If valid credentials exist in NVS namespace `chatai_net` (keys `ssid`, `password`), attempt the saved router connection directly. Do not scan first or ask the user to select a network again.
- Keep SoftAP available during STA association. Stop it only after STA has continuously held IPv4 for 60 seconds; LAN Web Admin must remain available through the router.
- If STA disconnects or loses IPv4, restore SoftAP immediately while STA reconnects. Retry SoftAP startup after a failure; retain diagnostics for failed recovery.
- If saved credentials are rejected by authentication/4-way handshake, report the failure and restore SoftAP so the user can correct the network.
- When the user submits replacement credentials, hold them as pending in RAM. Commit them to NVS only after STA receives IPv4. A failed attempt must leave the previously committed credentials intact. Do not require DNS, Internet probes, TLS, MQTT, WebSocket or AI-server availability before saving credentials.
- Wi-Fi router state, Internet-probe state and AI-service state are separate. DNS/TCP/AI failures must not mark STA disconnected, erase credentials, stop LAN Web Admin or disable local OTA.
- Web Admin must work over router LAN when STA has IPv4 and over setup SoftAP when router connectivity is unavailable. Local BIN upload/OTA must not require the ESP32 itself to have Internet access.
- Scan nearby Wi-Fi only when the user opens provisioning/recovery or explicitly requests a network change; do not scan during normal boot when saved credentials are available.
- Reveal/hide password controls are permitted; never return stored passwords from APIs or write them to logs.
- Keep the current APSTA transition synchronization, reconnect backoff and single-owner model. Do not introduce parallel radio-control tasks or replace the existing NetworkManager with another Wi-Fi library.

**Current source trace (reviewed on 2026-11-27; source inspection only):**
- `firmware/standalone/main/network_manager.cpp`: `NetworkManager::Initialize()` loads saved credentials, starts Wi-Fi, starts setup AP in APSTA mode and starts HTTP/recovery tasks.
- `EventHandler()`: STA start connects using saved credentials; STA disconnect/lost-IP and authentication rejection request SoftAP recovery; STA got-IPv4 marks router connectivity and commits pending credentials.
- `SavePendingCredentials()` / `SaveCredentials()`: replacement SSID/password are committed to `chatai_net` only after STA got IPv4.
- `WifiReconnectTask()`: uses the 60,000 ms stable-STA threshold before stopping SoftAP, and retries recovery/reconnect.
- `InternetCheckTask()`: probes external reachability separately; it must not change the router/Web Admin connected state.
- `StartProvisioning()` / `StopProvisioning()`: own APSTA/STA transitions and setup AP lifecycle.
- `firmware/standalone/main/web_admin.*`: local management, Wi-Fi provisioning/status/scan and OTA routes. Check current route behavior before modifying it.
- `docs/WEB_ADMIN.md`: detailed Web Admin contract and the ECC Shin Wi-Fi standard. Keep this guide and that document consistent.

**Validation state:** source-reviewed, **NOT BUILD-VERIFIED / NOT HARDWARE-VERIFIED**. Do not promote this architecture to VERIFIED until the tests below pass on the actual board and the evidence is recorded.

**Required Wi-Fi hardware tests:**
1. Cold boot and warm reboot with saved credentials: SoftAP appears immediately, STA reconnects without scanning, then SoftAP stops only after 60 seconds of stable IPv4; LAN Web Admin remains reachable.
2. Router power-off or out-of-range after AP has stopped: SoftAP returns immediately and STA reconnects when the router returns.
3. Enter a wrong replacement password: authentication error is visible, SoftAP remains/restores, and old NVS credentials remain unchanged (verify by reboot/reconnect).
4. Change the router password: old credentials are rejected, setup AP remains available, and the user can provision the new credentials.
5. Disable WAN/DNS or make Xiaozhi/Gemini unavailable: STA and LAN Web Admin remain connected; AI failure does not alter Wi-Fi state.
6. Upload a locally stored application BIN over LAN, then separately over SoftAP; verify OTA behavior on both paths only when OTA testing is explicitly authorized.
7. Confirm password show/hide, no password leakage in API responses/logs, and no unnecessary scan during normal boot.
8. Record firmware build, source commit, timestamps, relevant Wi-Fi diagnostics and outcome for every test.

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

> **ECC Shin / ChatAI TPIOS ESP32-S3:** Repository `Truongpink/esp32-claude`, work on `main` only. Current firmware integrates Xiaozhi v2.6.0 (submodule commit `4de00aed740595a8713b01f8332b9b2cf30685bc`) as the sole active voice client. ChatAI retains ownership of NetworkManager/Wi-Fi, Web Admin, AudioManager/I2S, OLED, buttons, diagnostics, NVS and manual dual-slot OTA; Xiaozhi uses ChatAI's AudioCodec bridge, no second Wi-Fi or I2S driver. **Wi-Fi standard:** on boot, start setup SoftAP + STA in APSTA mode; connect directly with saved NVS credentials without scanning; keep SoftAP until STA has stable IPv4 for 60 seconds; restore SoftAP immediately on STA disconnect/lost IPv4 or authentication rejection; commit replacement credentials only after STA receives IPv4; AI/DNS/TLS/WAN failure must not disconnect router Wi-Fi or disable LAN Web Admin/local OTA; scan only on explicit provisioning/network-change request. See the Wi-Fi architecture section in this guide and `docs/WEB_ADMIN.md`. This behavior is source-reviewed but not build- or hardware-verified. Current firmware integrates Xiaozhi v2.6.0 (submodule commit `4de00aed740595a8713b01f8332b9b2cf30685bc`) as the sole active voice client. Gemini is not active in current voice path. BLE disabled. ESP32-S3 N16R8, 16 MB flash/8 MB PSRAM; configured pins are in `docs/HARDWARE.md` and are not all physically verified. Build #80 succeeded in GitHub Actions run #97 on source commit `c251368ac66099c6846379440a6eeafb4c442c37`; user confirmed install and device runs “tạm ổn”, further stability testing pending. Read `docs/ECC_SHIN_PROJECT_CONTINUATION.md` then `docs/ECC_SHIN_XIAOZHI_MIGRATION_HISTORY.md`. Before edits inspect latest main HEAD and exact file SHA; minimal evidence-based changes; no branches; do not edit Xiaozhi submodule/pointer; do not run Actions/build/flash/OTA without separate explicit permission. Preserve Wi-Fi, Web Admin, AudioManager, OLED, NVS, partition and OTA; do not enable BLE or Gemini or claim WakeNet/AEC/NS without proof.
