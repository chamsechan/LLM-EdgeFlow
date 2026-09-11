---
name: llm-edgeflow-developer-guide
description: Route LLM-EdgeFlow implementation across Integration, Orchestration, Capability Nodes and Model Execution. Use for custom Nodes, platform I/O adapters and Operator bridges, Demo data conversion, Core, Models, Backends and verification; configuration-only solution work belongs to pipeline-composer.
---

# LLM-EdgeFlow Developer Guide

First classify the requested change. Read only the references needed for the affected layer; do not load every reference by default.

- New modality, C ABI structure/function behavior, Adapter, Operator bridge, or allowed runtime Pipeline name: read [Integration](references/integration.md).
- Demo dataset/carrier construction, result display or registration: follow [business onboarding](../../../doc/dev_guide/business_onboarding.md#统一-demo-接入). External request parsing and response assembly belong to Adapter work; load Integration for those changes even when the C carrier layout stays the same.
- Pipeline lifecycle, Validator, DAG planning, `AlgContext`, `BlackboardKey`, or session behavior: read [Orchestration](references/orchestration.md).
- New or modified capability Node, its parameters, or a Control handler: read [Capability Nodes](references/capability-nodes.md). Start Control work from the [compiled example](../../../doc/dev_guide/first_control.md); reuse transport and instance routing.
- Parameter values or compatible model replacement with no implementation changes: use `pipeline-composer` and [native deployment inspection](../../../doc/VERIFIABLE_SELECTION.md#替换模型后确认实际生效配置).
- New Model semantics/capability, inference Backend, neutral protocol, or batch behavior: read [Model Execution](references/model-execution.md).
- Before completing any implementation, read [Verification](references/verification.md).

Multi-layer features must preserve the dependency direction Integration → Orchestration → Capability Nodes → Model Execution. Never introduce an upward dependency. Follow [`CONTRIBUTING.md`](../../../CONTRIBUTING.md) for RFC thresholds, branch lifecycle, documentation, verification, and delivery.

If the request only configures a solution using existing nodes and biz contracts, use
`pipeline-composer`, including necessary `.conf` and optional Profile edits. Ordinary composition
must not modify Core or node implementations.

Use `github-branch-merge` only when the user explicitly asks to upload, open a PR, or merge.

After closing a capability or conversion gap, rebuild the Catalog and return to composition:
validate and run the user's intended Pipeline with its own `.conf` and expected results. A compiled
Node alone does not complete a request for a working solution. Routine custom Nodes that preserve
existing contracts follow the lightweight path in `CONTRIBUTING.md`; add no extra approval step.
