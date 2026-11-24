# ChatAI TPIOS — Issues & Handoff

## Current known verification limits
- Physical board pin routing and complete audio/Gemini conversation: NOT TESTED by this documentation integration.
- Installed partition table on the physical device: NOT TESTED independently.
- Repository README documents direct Gemini Live, but any future change must verify current source and configuration rather than relying on prose alone.
- Web Admin uses HTTP and NVS encryption is not enabled according to README; keep it on a trusted LAN.

## Handoff checklist
- [ ] Confirm current branch is `main`.
- [ ] Read root `AGENTS.md` and ECC Shin Core.
- [ ] Load all five project memory files.
- [ ] Inspect current source and configuration related to the task.
- [ ] Label evidence accurately.
- [ ] Present exact file scope, risks and validation plan.
- [ ] Obtain approval before changes.
- [ ] Obtain separate approval before Actions/build/flash/OTA/deploy.
- [ ] Review diff and report commit plus untested areas.

## Stop conditions
Stop and ask if repository HEAD changed unexpectedly, branch is not `main`, relevant evidence is missing, Web Admin baseline may be affected, or the requested scope exceeds approval.
