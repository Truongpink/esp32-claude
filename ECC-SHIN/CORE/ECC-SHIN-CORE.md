# ECC Shin Core

**Project:** ChatAI TPIOS — ESP32-S3  
**Repository:** https://github.com/Truongpink/esp32-claude  
**Branch policy:** `main` only  
**Permission mode:** ASK before every modification; standing approval for commits to `main` after the change itself is approved

## Mission
ECC Shin is the project-level engineering governance and workflow system for ChatAI TPIOS. It coordinates specialist responsibilities, maintains evidence-based project memory, and protects existing firmware behavior. It is a documented workflow for the assistant and contributors, not a claim that 12 independent AI agents or autonomous services are running.

## Operating principles
1. Read `AGENTS.md` and the relevant project memory before each task.
2. Verify current `main` HEAD and inspect the exact source/configuration before proposing edits.
3. Distinguish VERIFIED, USER-REPORTED, ASSUMED, BLOCKED and NOT TESTED.
4. Prefer the smallest change that satisfies the approved task.
5. Obtain explicit approval before creating, editing or deleting files and before changing configuration/data. Approval is limited to its stated scope.
6. Standing commit authorization: after the user has approved the exact source/documentation changes, the assistant may commit those approved changes directly to `main` without asking again for commit permission. Review the diff first and report the commit SHA and URL.
6. Preserve working firmware, network provisioning, Web Admin compatibility, audio, OLED, NVS and OTA behavior.
7. Never expose credentials, API keys, tokens or secrets.
9. Do not run Actions, CI, builds, flash, OTA or deployment without separate explicit authorization; commit authorization does not include these operations.
10. Never create branches, force-push, reset history or expand the approved change scope.
11. Review the resulting diff and report exactly what was and was not verified.

## Error handling and bug fixing (mandatory)

When an error is detected, ECC Shin must solve the defect—not default to adding logs and asking the user to reproduce it again.

1. **Inspect carefully:** establish the exact symptom, expected/actual behavior, reproduction conditions, relevant logs, current source revision, configuration and execution path. Find the earliest divergence and distinguish VERIFIED facts from hypotheses.
2. **Fix when evidence is sufficient:** when the root cause is supported strongly enough to distinguish it from plausible alternatives, prepare the smallest root-cause fix. Do not stop at source inspection plus extra logging, and do not make speculative changes.
3. **Fix and verify observability in the same change:** alongside the fix, add or adjust targeted diagnostic logs at the relevant boundary so the next run can confirm the fix's effect. Prefer clear success/failure outcomes, relevant error codes and state transitions. Reuse adequate existing logs when they already prove the needed outcome; do not create noisy or redundant logs. Never log secrets or sensitive user data.
4. **If evidence is insufficient:** do not guess or patch blindly. Add only the narrow diagnostic or test needed to discriminate the remaining hypotheses. State exactly what evidence is missing, what the new log/test will establish, and how each possible result changes the fix. This is the exception—not the default workflow.
5. **Respect approval boundaries:** explain exact files, behavior changes, risks and validation plan. ASK still applies before every file/configuration/device-state modification. A request to investigate does not automatically authorize a fix. Implement only the scope explicitly approved.
6. **Verify and report honestly:** inspect the final diff; check that diagnostic events are useful and do not expose sensitive data. Run only tests/builds/device checks separately authorized by the user. Distinguish source review, static checks, build, runtime and hardware verification; never claim an unperformed test passed.

**Default order:** inspect evidence → identify root cause → implement the minimal fix + targeted verification log in the same approved change → review diff → validate with authorized tests → report results and remaining uncertainty.

**Forbidden default:** inspect source → add generic logs only → ask the user to rerun → postpone a fix despite already having sufficient evidence.

## Task lifecycle
INTAKE → LOAD PROJECT MEMORY → SELECT SPECIALIST ROLE → INSPECT EVIDENCE → PLAN & RISK REVIEW → ASK APPROVAL FOR FILE/DATA CHANGES → IMPLEMENT APPROVED SCOPE → DIFF REVIEW → COMMIT APPROVED CHANGES TO MAIN → REPORT & HANDOFF.

If permission to modify files, source evidence, or repository state is unclear, stop and ask. Do not ask again for commit permission when the exact changes have already been approved; standing authorization covers that commit only. Never infer permission to modify files from a broad goal.

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
