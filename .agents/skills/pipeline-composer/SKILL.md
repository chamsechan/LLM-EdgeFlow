---
name: pipeline-composer
description: Compose or adjust Pipeline JSON, deployment conf, and Demo Profiles using existing LLM-EdgeFlow capabilities; validate and run the edited solution.
---

# Pipeline Composer

Use the target build's runtime Catalog, Validator, and native Resolver for capabilities, ports,
parameters, biz contracts, and deployment semantics. Do not maintain a parallel catalog or
validation logic in this skill. Scope edits to the requested JSON, necessary `.conf`, and
optional Profile; configuration-only work normally needs no RFC.

Choose the applicable part of [the command reference](references/workflow.md): discover assets,
create/clone a solution, validate changes, or run the intended configuration. Reuse current
results for unchanged inputs and the same target build; rebuild when binaries or registrations
are missing/stale. Do not read unrelated examples or repeat an unchanged full workflow.

The boundary is the complete Operator SDK request/response, not Node ports or just a shared
carrier type. Missing external parsing, field selection, serialization, or response assembly
belongs in registered Adapter converters, never Demo/Python. A capability or I/O gap routes to
[the developer guide](../llm-edgeflow-developer-guide/SKILL.md); do not silently implement C++
as configuration work. After resolving the gap, resume the requested solution rather than
stopping at a handoff. JSON prompt solutions also use
[the JSON contract guide](../json-prompt-solution/SKILL.md).

For a requested runnable solution, validate and inspect the edited Pipeline's plan, confirm
its `.conf` resolves to that JSON, run it, and inspect IDs/status/output fields. Report static
validation, executed results, and real-model/target-hardware acceptance separately. Missing
assets leave execution unverified; test-only registrations must not mask a production failure.
Use [effects verification](../../../doc/VERIFIABLE_SELECTION.md) when quality acceptance is
requested. [CONTRIBUTING.md](../../../CONTRIBUTING.md) owns focused checks, the single final gate,
and delivery authorization; do not invoke remote delivery from composition alone.
