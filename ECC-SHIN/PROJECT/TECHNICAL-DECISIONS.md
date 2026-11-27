# ChatAI TPIOS — Technical Decisions

| Decision / constraint | Status | Rationale / evidence |
|---|---|---|
| Use standalone ESP-IDF firmware | VERIFIED (docs) | README identifies `firmware/standalone` and ESP-IDF 6.1 |
| Target ESP32-S3 | VERIFIED (docs) | README project target |
| Work on `main` only | VERIFIED (AGENTS.md) | Repository rule |
| Protect current Web Admin UI | VERIFIED (AGENTS.md) | Approved baseline; explicit approval required for UI changes |
| Keep API keys out of logs/source/browser status responses | VERIFIED (AGENTS.md/README) | Security requirement |
| Local Web Admin remains LAN-only | VERIFIED (README guidance) | HTTP and NVS encryption disabled per README |
| Router Wi-Fi state is independent of AI-server probes | IMPLEMENTED (source review; NOT TESTED) | STA IPv4 is the router success criterion; DNS/TCP checks must not mutate Wi-Fi setup state |
| Saved Wi-Fi credentials are tried directly at boot | IMPLEMENTED (source review; NOT TESTED) | Initialize loads NVS credentials and STA_START initiates connection without scanning |
| Authentication failure restores provisioning access | IMPLEMENTED (source review; NOT TESTED) | Authentication/4-way-handshake rejection requests immediate SoftAP recovery |
| Failed replacement credentials preserve prior NVS values | IMPLEMENTED (source review; NOT TESTED) | Credentials are written only after the new STA receives IPv4 |
| Wi-Fi password visibility control exists in provisioning and admin pages | IMPLEMENTED (source review; NOT TESTED) | Both password inputs have explicit show/hide controls |
| No Actions/build/flash/OTA without separate approval | VERIFIED (AGENTS.md) | Permission boundary |
| Physical partition table matches documented 16 MB dual-slot layout | NOT TESTED | README says board table not independently read back |

## Decision policy
Do not resolve contradictory documentation or change protocol/storage choices by assumption. Inspect source and configuration, document evidence, then ask for a decision when alternatives remain.
