# ChatAI TPIOS standalone migration

## Current baseline

- Existing production path: the repository's manual workflow downloads pinned
  XiaoZhi v2.6.0 and applies ChatAI patches before building.
- New independent project: `firmware/standalone/`.
- The standalone project initializes NVS, ESP-NETIF, the default ESP event loop,
  and a ChatAI-owned native ESP-IDF Wi-Fi station manager.
- Wi-Fi credentials are stored in the `chatai_net` NVS namespace. The new manager
  does not erase NVS or log credentials.
- When no usable credentials exist, it starts a WPA2-protected setup AP with a
  per-boot random password and a local HTTP form at `192.168.4.1`. The AP SSID and
  password are exposed only through NetworkManager getters for the OLED UI; they
  are never written to logs or NVS. The AP stops after the station connects.
- A standalone SSD1306 text display uses ESP-IDF's I2C master driver directly;
  it shows the setup AP SSID/password and `192.168.4.1`, or the station IP after
  connection. It uses GPIO 41/42 and address 0x3C from the existing board profile.
  These pins and the display remain unverified on the physical device.
- A standalone ESP-IDF I2S manager now owns speaker TX on I2S0 (GPIO 15/16/7,
  24 kHz PCM16 carried in 32-bit I2S slots) and microphone RX on I2S1 (provisional
  GPIO 5/4/6, 16 kHz). Microphone samples are read as 32-bit slots and converted
  to PCM16. GPIO 17 controls the
  speaker amplifier. The microphone pin mapping is not verified on hardware.
- The I2S manager exposes a synchronous speaker-reference PCM callback at 24 kHz
  for a future AEC adapter. AEC and 24-to-16 kHz reference resampling are not yet
  integrated in the standalone firmware; the legacy ESP-SR AEC path is untouched.
- No XiaoZhi source, headers, components, or cloud services are linked by the
  standalone project.
- Existing firmware, scripts, OTA flow, and manual-only workflow are unchanged.
- A ChatAI-owned Web Admin now shares the native HTTP server with Wi-Fi provisioning.
  The server remains active after station connection; `/admin` provides local
  Gemini API Key save/replace/delete controls.
- Gemini API Key is stored in NVS namespace `chatai_ai`, key `gemini_key`.
  The status API returns only a configured boolean; the key is never returned to
  the browser or logged. Mutating routes check the request Origin and a per-boot
  CSRF token.
- Web Admin is plain local HTTP and NVS encryption is not configured. This is
  acceptable only for a trusted local network in the current personal-device
  design; do not expose the management page to the public Internet.
  Origin/CSRF checks are cross-site request protections, not user authentication.
  Any client that can reach the device on the local network can manage the key; an
  administrator login is not included in this first implementation slice.

## Migration sequence

1. **Platform foundation — started:** independent ESP-IDF project and core platform
   initialization with NVS preservation.
2. **Network ownership — first slice added:** native station initialization,
   persisted SSID/password, connection state, IPv4 reporting, and bounded reconnect
   attempts. A WPA2-protected setup AP, local HTTP form, and standalone SSD1306
   status screen are implemented in the independent project.
3. **Hardware/audio ownership — first slice added:** OLED text output and native
   I2S PCM capture/playback now use ChatAI-owned interfaces. The speaker-reference
   callback is the integration point for AEC; resampling and AEC remain pending.
   Buttons and hardware pin verification are still pending.
4. **Gemini Live ownership — first audio slice added:** a standalone WebSocket client
   connects directly to Gemini Live, waits for `setupComplete`, streams mono PCM16
   16 kHz microphone chunks through `realtimeInput.audio`, and queues 24 kHz PCM16
   model audio for I2S playback. Server-side VAD is retained. AEC, interruption
   timing measurements, session recovery validation, and hardware verification
   remain pending. No XiaoZhi `Protocol`, `AudioService`, or `Board` singleton is used.
5. **Web Admin and OTA — first slice added:** Gemini API Key save/replace/delete is
   implemented in the standalone Web Admin and NVS. Remaining device controls and
   safe inactive-slot OTA are still pending.
6. **Integration and removal:** verify all functions on hardware, then remove the
   XiaoZhi download/patch build path and unused dependencies.

## Non-negotiable safety constraints

- Work on `main` only; never create another branch.
- Do not run GitHub Actions, CI, firmware builds, or flash operations without
  separate explicit user permission.
- Do not change the existing workflow as part of this migration.
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
