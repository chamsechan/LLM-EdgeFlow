---
name: llm-edgeflow-developer-guide
description: Implement or fix LLM-EdgeFlow C++/Demo behavior in Adapter, Core, Nodes, Models, or Backends. Not configuration-only composition.
---

# LLM-EdgeFlow Developer Guide

Read the affected layer below, not the entire reference set. `AGENTS.md` owns shared constraints
and roles; [CONTRIBUTING.md](../../../CONTRIBUTING.md) owns RFC thresholds, local iteration,
phase acceptance, verification, and delivery. Follow its [RFC lookup](../../../CONTRIBUTING.md#rfc-lookup)
policy for conditional reading; RFC citations in layer guides are not prerequisites.

| Affected behavior | Read |
| :--- | :--- |
| Operator SDK, modality, external payload, Converter, IoBinding, allowed Pipeline names | [Integration](references/integration.md) |
| Pipeline lifecycle, Validator/planning, typed Blackboard, sessions | [Orchestration](references/orchestration.md) |
| Capability Node, parameters, Control handler | [Capability Nodes](references/capability-nodes.md); for Control, [compiled example](../../../doc/dev_guide/first_control.md) |
| Model semantics/capability, Backend/protocol, batching | [Model Execution](references/model-execution.md) |
| Demo carriers, dataset, registration, result display | [Demo onboarding](../../../doc/dev_guide/business_onboarding.md#统一-demo-接入) |

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
