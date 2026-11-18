# ChatAI TPIOS — current project checkpoint

Last reviewed: 2026-11-19

## Repository state

- Repository: `Truongpink/esp32-claude`
- Active branch: `main`
- Current HEAD before this cleanup: `1dfcf53ecc110497d374bebcff5e1b16f3d6dc82`
- Only maintained firmware: `firmware/standalone`
- Toolchain: ESP-IDF 6.1, target ESP32-S3.
- The former upstream-download and overlay firmware path has been retired.

## Architecture

- ChatAI owns Wi-Fi, network provisioning, NVS, OLED, I2S audio, Web Admin, and diagnostics.
- Gemini Live is the only AI service and is connected directly over WSS/TLS.
- Gemini API Key is entered in local Web Admin and stored in NVS namespace `chatai_ai`, key `gemini_key`.
- No Cloudflare Worker, token broker, VPS, proxy, or Xiaozhi service is used.
- Microphone format: mono PCM16, 16 kHz.
- Speaker format: mono PCM16, 24 kHz.
- Server-side VAD is used; local WakeNet, MultiNet, and local VAD are not part of the design.

## Build and output

- The only supported GitHub Actions workflow is `.github/workflows/build-standalone.yml`.
- Trigger: `workflow_dispatch` only.
- Published output: one application image, `ChatAI_TPIOS.bin`.
- GitHub Actions wraps downloadable artifacts in a ZIP container; the archive contains only the BIN.
- The current partition defaults use a 1.5 MB single-app layout. OTA is not implemented.
- A first-time USB install requires a compatible bootloader and partition table supplied separately.

## Recent build record

- Workflow run: [44334411807](https://github.com/Truongpink/esp32-claude/actions/runs/44334411807), attempt 3, completed successfully.
- Important limitation: that run used workflow head SHA `66e6a5f39be69e42a505476674a0e3dcce72f225`, while `main` had advanced to `1dfcf53ecc110497d374bebcff5e1b16f3d6dc82`. Do not treat the run as verification of the newer partition configuration.
- No build is started by this cleanup. A new build requires separate explicit user permission.

## Hardware validation status

- OLED, microphone GPIO mapping, speaker path, and full Gemini Live conversation still require confirmation on the actual device.
- Microphone mapping GPIO 5/4/6 is provisional.
- NVS encryption is not enabled.
- Web Admin uses plain HTTP and must remain on a trusted local network.
- Do not infer hardware success from a successful CI compile.

## Rules for future work

- Work on `main` only; never create or switch branches.
- Inspect the actual branch HEAD and relevant source before every edit.
- Make minimal targeted changes and preserve Wi-Fi, Web Admin, NVS, Gemini, audio, OLED, TLS, and diagnostics.
- Never run or rerun Actions, builds, or flash operations without explicit permission for that specific operation.
- Never add automatic push/PR workflow triggers.
- Never expose the Gemini API Key in logs, OLED, source, or GitHub.
