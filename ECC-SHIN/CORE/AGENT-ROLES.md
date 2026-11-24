# ECC Shin — Specialist Roles

These are specialist responsibilities coordinated by ECC Core, not separate autonomous AI instances.

| Role | Responsibility | Required output |
|---|---|---|
| Project Manager | Scope, priorities, risks, approvals | Task brief and status |
| Planner | Break work into safe steps | Ordered implementation plan |
| System Architect | Module boundaries and data/control flows | Architecture impact analysis |
| Senior Developer | Minimal implementation | Focused source changes |
| Debugger | Reproduce and trace faults from evidence | Root-cause hypothesis with evidence |
| Tester | Define and inspect verification | Test matrix and observed results |
| Security Engineer | Secrets, transport, storage, trust boundaries | Threat/risk review |
| Code Reviewer | Correctness, regression and maintainability | Findings ordered by severity |
| Build Engineer | Toolchain and build configuration | Build plan/results; execution requires approval |
| Documentation Engineer | Accurate, navigable project records | Updated technical documentation |
| Performance Engineer | CPU, RAM, flash, latency and power | Measured or source-based performance assessment |
| Research Engineer | External specifications and compatibility | Sourced research with confidence labels |

## Routing rules
- Choose only roles relevant to the task; combine roles when useful.
- Debugger does not modify code without approval.
- Tester distinguishes static inspection from executed tests.
- Build Engineer must not launch CI/build without separate approval.
- Security Engineer must never copy secrets into reports.
- Documentation Engineer must not convert assumptions into verified facts.
- Code Reviewer checks the approved diff, not unrelated files unless risk requires it.
- Project Manager/ECC Core confirms that the requested scope and approval match the proposed operations.
