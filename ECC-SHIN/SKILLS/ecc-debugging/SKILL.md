# Skill: ecc-debugging

## Objective
Resolve the underlying defect with evidence. Diagnostic logging is part of a fix's verification—not a substitute for fixing a cause that is already sufficiently established.

## Required workflow
1. Capture the exact symptom, expected behavior, actual behavior, reproduction conditions, firmware/software revision and relevant configuration.
2. Read current source and existing logs; trace the real execution path to the earliest divergence. Do not infer implementation from stale notes or function names.
3. Separate VERIFIED observations from USER-REPORTED information and ASSUMED hypotheses. Rank alternatives and identify what evidence distinguishes them.
4. **If root cause is sufficiently supported:** propose the smallest root-cause correction. In the same approved change, add or refine targeted logs that show the relevant state transition and whether the corrected path succeeds or fails. Include useful error codes/context, but never secrets or noisy repetitive data.
5. **If root cause is not sufficiently supported:** do not guess. Propose only a narrowly scoped diagnostic/test that answers a specific unresolved question. Explain the missing evidence and how its outcomes will guide the fix. Do not use generic logging as a default delay tactic.
6. State exact files, behavior impact, risks and validation plan. Obtain approval before modifying code, configuration or device state; execute only the approved scope.
7. Review the diff for correctness, scope, regressions and log quality. Run only separately authorized tests/build/device checks.
8. Report what was fixed, what the new/adjusted logs verify, actual test evidence, and what remains NOT TESTED. Never claim root cause or runtime success beyond the evidence.

## Decision rule
- **Enough evidence → fix + targeted verification logging together.**
- **Not enough evidence → targeted evidence collection first, then fix as soon as the evidence is sufficient.**
- Do not stop after adding logs when a justified fix can already be made.
