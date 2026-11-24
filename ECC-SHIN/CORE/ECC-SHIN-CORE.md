# ECC Shin Core

**Project:** ChatAI TPIOS — ESP32-S3  
**Repository:** https://github.com/Truongpink/esp32-claude  
**Branch policy:** `main` only  
**Permission mode:** ASK before every change

## Mission
ECC Shin is the project-level engineering governance and workflow system for ChatAI TPIOS. It coordinates specialist responsibilities, maintains evidence-based project memory, and protects existing firmware behavior. It is a documented workflow for the assistant and contributors, not a claim that 12 independent AI agents or autonomous services are running.

## Operating principles
1. Read `AGENTS.md` and the relevant project memory before each task.
2. Verify current `main` HEAD and inspect the exact source/configuration before proposing edits.
3. Distinguish VERIFIED, USER-REPORTED, ASSUMED, BLOCKED and NOT TESTED.
4. Prefer the smallest change that satisfies the approved task.
5. Obtain explicit approval before each change operation. Approval is limited to its stated scope.
6. Preserve working firmware, network provisioning, Web Admin compatibility, audio, OLED, NVS and OTA behavior.
7. Never expose credentials, API keys, tokens or secrets.
8. Do not run Actions, CI, builds, flash, OTA or deployment without separate explicit authorization.
9. Review the resulting diff and report exactly what was and was not verified.

## Task lifecycle
INTAKE → LOAD PROJECT MEMORY → SELECT SPECIALIST ROLE → INSPECT EVIDENCE → PLAN & RISK REVIEW → ASK APPROVAL → IMPLEMENT APPROVED SCOPE → SOURCE REVIEW → REPORT & HANDOFF.

If permission, source evidence, or repository state is unclear, stop and ask. Never infer permission from a broad goal.

## Project-specific guardrails
- Work on `main` only; never create or switch branches.
- Treat the current Web Admin UI as an approved baseline. Do not alter its HTML, CSS, JS, layout, labels, navigation, endpoints or interactions without specific approval.
- Do not change firmware as part of documentation-only tasks.
- Do not claim a build, runtime or hardware test passed unless evidence exists.
- Keep documentation consistent with source; flag unresolved conflicts instead of silently choosing a side.

## Evidence labels
- **VERIFIED:** directly confirmed from current repository/source or an executed, evidenced test.
- **USER-REPORTED:** supplied by the user, not independently verified.
- **ASSUMED:** working hypothesis; must not be presented as fact.
- **BLOCKED:** contradictory, inaccessible or insufficient evidence prevents a conclusion.
- **NOT TESTED:** no relevant test has been run.

## Specialist routing
Use `AGENT-ROLES.md` to select one or more specialist perspectives. Use `SKILL-CATALOG.md` to select a repeatable workflow. ECC Core remains accountable for scope, permission and final reporting.
