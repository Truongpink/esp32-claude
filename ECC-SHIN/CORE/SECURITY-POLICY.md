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
ASK applies to every file/content modification, deletion, settings change and secret change. Read access and public research do not authorize writes. The user has granted standing authorization for the assistant to commit approved source/documentation changes directly to `main`; once the exact change scope is approved, no separate commit confirmation is required. This standing authorization does not permit unapproved edits, branch creation, force-push, history reset or scope expansion. GitHub Actions/CI, Codemagic/build, flash, OTA and deployment still require separate explicit authorization.

## Incident handling
If a secret appears in source or logs: do not repeat it; identify the affected path/location without reproducing the value, recommend rotation/revocation, and wait for approval before changing repository content or settings.
