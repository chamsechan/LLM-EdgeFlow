---
name: llm-edgeflow-developer-guide
description: Route LLM-EdgeFlow cross-component C++ changes to the owning development skill; handle Core/Pipeline lifecycle, Validator, and Demo implementation. Use the specific Adapter, Node, Model or Backend skill when the component is known.
---

# LLM-EdgeFlow Developer Guide

Read the affected skill/reference below, not the entire set. `AGENTS.md` owns shared constraints
and roles; [CONTRIBUTING.md](../../../CONTRIBUTING.md) owns design review criteria, local iteration,
phase acceptance, verification, and delivery. Follow its
[design and current-contract policy](../../../CONTRIBUTING.md#3-design-and-current-contracts);
start with current guides and affected code/tests.

| Affected behavior | Entry |
| :--- | :--- |
| Business requirements needing component selection and a DAG | [Solution planner](../edgeflow-solution-planner/SKILL.md) |
| Operator SDK, external payload, Converter, IoBinding | [Adapter developer](../edgeflow-adapter-developer/SKILL.md) |
| One input item to one output item, pure computation | [Map Node developer](../edgeflow-node-map-developer/SKILL.md) |
| Text preprocessing → one LLM call → text postprocessing | [LLM Node developer](../edgeflow-node-llm-developer/SKILL.md) |
| Multiple ports/models, derived outputs, configurable sampling, batch algorithms | [Batch Node developer](../edgeflow-node-batch-developer/SKILL.md) |
| Model preprocessing, semantics and capabilities | [Model developer](../edgeflow-model-developer/SKILL.md) |
| Vendor runtime, execution protocol implementation and resources | [Backend developer](../edgeflow-backend-developer/SKILL.md) |
| Pipeline lifecycle, Validator/planning, typed Blackboard, sessions | [Orchestration](references/orchestration.md) |
| Demo carriers, dataset, registration, result display | [Demo onboarding](../../../doc/dev_guide/business_onboarding.md#5-统一-demo-接入) |

Map, LLM and Batch are authoring forms of the same runtime. Common versus custom identifies
ownership, not a second set of authoring interfaces. For existing Node parameter/Control work,
read [shared Node contracts](references/capability-nodes.md) and only the applicable form;
use the [compiled Control example](../../../doc/dev_guide/first_control.md) when needed.

External field selection/response assembly is Integration work even when the carrier layout
stays unchanged; Demo must not replace Adapter conversion. Load all affected-layer contracts
for a cross-layer change, while preserving the downward dependency direction.

For parameter values or compatible model replacement without implementation, use
[pipeline-composer](../pipeline-composer/SKILL.md); inspect
[native deployment resolution](../../../doc/VERIFIABLE_SELECTION.md#替换模型后确认实际生效配置)
when model/deployment selection changes.

For changed runtime, Catalog, or business I/O behavior, select the applicable evidence from
[Verification](references/verification.md). A documentation-only correction does not require
unrelated Pipeline execution; the CONTRIBUTING final gate still applies.

After closing a capability/conversion gap for a requested solution, rebuild the affected
registrations and resume composition, validation, and execution of the user's own configuration.
A compiled Node or routing handoff alone is not completion. Routine custom Nodes preserving
existing contracts use the lightweight CONTRIBUTING path, with no extra approval step.
