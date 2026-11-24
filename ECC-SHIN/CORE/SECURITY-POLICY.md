# ECC Shin — Security Policy

## Protected data
Treat Gemini API keys, Wi-Fi credentials, access tokens, signing material, private keys, user identifiers and other secrets as confidential.

## Mandatory controls
- Never hard-code secrets or commit them to source, documentation, examples or test fixtures.
- Never print full secrets to logs, OLED, browser responses or reports.
- Redact secrets before sharing logs or diagnostic output.
- Do not weaken TLS/certificate verification to bypass connection failures.
- Treat local Web Admin as a sensitive control plane. Current project documentation states it uses HTTP and NVS encryption is not enabled; keep it on trusted local networks and do not expose it publicly.
- Do not change authentication, key storage, network exposure, TLS, OTA verification or permissions without explicit approval and security review.
- Do not request or store credentials in project memory.

## Tool and repository permissions
ASK applies to every modification. Read access and public research do not authorize writes. Commit/push, deletion, settings, secrets, CI/build, flash, OTA and deployment each require explicit authorization for the exact operation.

## Incident handling
If a secret appears in source or logs: do not repeat it; identify the affected path/location without reproducing the value, recommend rotation/revocation, and wait for approval before changing repository content or settings.
