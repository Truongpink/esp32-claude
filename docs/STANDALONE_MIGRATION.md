# ChatAI TPIOS standalone migration

## Current baseline

- Existing production path: the repository's manual workflow downloads pinned
  XiaoZhi v2.6.0 and applies ChatAI patches before building.
- New independent project: `firmware/standalone/`.
- The new project currently initializes NVS, ESP-NETIF, and the default ESP event
  loop using ESP-IDF APIs only.
- No XiaoZhi source, headers, components, or cloud services are linked by the
  standalone project.
- Existing firmware, scripts, OTA flow, and manual-only workflow are unchanged.

## Migration sequence

1. **Platform foundation — started:** create the independent ESP-IDF project and
   initialize core platform services without erasing NVS on errors.
2. **Network ownership:** implement ChatAI Wi-Fi station/AP management with native
   ESP-IDF Wi-Fi, event handling, and persisted configuration.
3. **Hardware/audio ownership:** move I2S, GPIO, buttons, OLED, PCM capture/playback,
   and the currently required AEC path behind ChatAI-owned interfaces.
4. **Gemini Live ownership:** move the existing wire-protocol behavior into a
   standalone client that does not inherit XiaoZhi `Protocol` or use
   `AudioService` / `Board` singletons.
5. **Web Admin and OTA:** port the existing controls and safe inactive-slot OTA
   behavior to the standalone services.
6. **Integration and removal:** verify all functions on hardware, then remove the
   XiaoZhi download/patch build path and unused dependencies.

## Non-negotiable safety constraints

- Work on `main` only; never create another branch.
- Do not run GitHub Actions, CI, firmware builds, or flash operations without
  separate explicit user permission.
- Do not change the existing workflow as part of this scaffold.
- Do not erase NVS automatically; preserve existing user configuration.
- Do not change the partition table or assume flash geometry before reading the
  actual device partition table.
- Do not remove the current firmware path or dependencies until the standalone
  replacement has been verified on the target board.
- Keep Gemini audio at mono PCM16 16 kHz input and mono PCM16 24 kHz output unless
  measured hardware behavior requires a documented conversion.
- Do not re-enable local WakeNet, MultiNet, or local VAD in the target architecture.
- Keep ESP-SR AEC until a replacement is measured and proven; Gemini Live is not
  assumed to provide acoustic echo cancellation.
