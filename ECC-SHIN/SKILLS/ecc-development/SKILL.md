# Skill: ecc-development

1. Read project rules and inspect the exact target files on current `main`.
2. Confirm explicit approval for each proposed change.
3. Prefer native ESP-IDF/C++ facilities and existing project patterns; avoid unnecessary dependencies.
4. Make the smallest coherent change; preserve public interfaces and working behavior.
5. Do not touch Web Admin UI or unrelated firmware paths without specific approval.
6. Inspect the final diff for accidental changes, secrets and formatting issues.
7. Report source-level verification separately from build/runtime/hardware verification.
