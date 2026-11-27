# ECC Shin — Release Changelog

## 1.0.0 — Initial repository integration
- Added ECC Shin core governance, 12 specialist role definitions and a 10-skill workflow catalog.
- Added 10 documented skill procedures.
- Added ChatAI TPIOS project memory: project, architecture, technical decisions, progress and handoff.
- Added repository navigation and integration guidance.
- Preserved firmware source and existing Web Admin implementation.
- No build, GitHub Actions, hardware test, flash or OTA operation was run as part of this documentation integration.

## Change policy
Record future ECC Shin documentation changes here with date/version, changed paths, reason and verification status. Do not claim firmware release status from a documentation-only change.


## 1.1 — Standing commit authorization
- User granted standing permission to commit source/documentation changes directly to `main` after approving the exact change scope.
- No separate confirmation is required for that commit; review the diff and report the commit SHA and URL.
- File edits still require prior approval. No branch creation, force-push, history reset, scope expansion, build, Actions, flash, OTA or deployment is authorized by this rule.


## 1.2 — Root-cause fixes with verification logging (2026-11-27)
- Added a mandatory error-handling rule: investigate carefully, then fix the evidenced root cause and add targeted verification logging in the same approved change.
- Clarified that logging-only evidence collection is reserved for cases where the available evidence cannot yet distinguish the cause; diagnostics must answer a specific question.
- Updated the debugging skill and Vietnamese user guide to match ECC Core.
- Documentation-only change. No firmware source, build, GitHub Actions, flash, OTA or hardware behavior was changed or tested.
