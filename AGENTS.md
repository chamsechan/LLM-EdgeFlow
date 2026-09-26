# LLM-EdgeFlow Agent Governance

This file owns shared guardrails, task routing, and model-agnostic agent responsibilities.
[CONTRIBUTING.md](CONTRIBUTING.md) owns the development and delivery lifecycle; affected-layer
references below own detailed implementation contracts. Do not duplicate these rules in
provider configurations or load every linked document at task start.

## Start with the requested scope

Read the files and tests needed to resolve the current task. A contained documentation, test,
or configuration edit does not require a repository-wide tour. Read the relevant CONTRIBUTING
sections before editing or delivering.

- Existing-capability Pipeline JSON, deployment `.conf`, or optional Demo Profile changes:
  [pipeline-composer](.agents/skills/pipeline-composer/SKILL.md).
- Complete JSON request → prompt processing → JSON response solutions:
  [json-prompt-solution](.agents/skills/json-prompt-solution/SKILL.md), then its relevant route.
- Operator/Adapter, Core/Pipeline, Node, Model, Backend, or Demo implementation:
  [llm-edgeflow-developer-guide](.agents/skills/llm-edgeflow-developer-guide/SKILL.md).
  Read only affected-layer references, including every layer of a cross-layer change.
- Architecture or contract decisions: use
  [design and current contracts](CONTRIBUTING.md#3-design-and-current-contracts).
  Start from current guides and affected code/tests; consult Git history only when needed.
- Upload, PR, or merge explicitly requested by the user:
  [github-branch-merge](.agents/skills/github-branch-merge/SKILL.md).
  An implementation request alone never authorizes remote delivery.

Ordinary solution work belongs in Pipelines, reusable business Nodes, platform I/O, and the
shared Demo; change Core, Models, or Backends only for a demonstrated gap. Query the target
build's `alg_pipeline_tool` when capability/configuration facts are needed. Registrations and
Definitions, not prose, are the executable Catalog; unrelated edits need no Catalog query.

## Architecture guardrails

Use the canonical responsibility names in active docs, diagnostics, and build targets:
接入适配层 / Integration → 流程编排层 / Orchestration → 能力节点层 / Capability Nodes
→ 模型执行层 / Model Execution. Dependencies flow downward only.

- **Integration:** the C++ Operator API (`llm_edgeflow::operator_api`) is the sole public
  algorithm interface. Exported table functions retain `noexcept` and both
  `catch (const std::exception&)` and `catch (...)` barriers. Registered `InputConverter`,
  `OutputConverter`, and `IoBinding` own biz conversion, not central dispatch or lower layers.
  The complete external request/response is the SDK contract: validation/field selection and
  response assembly/capacity/serialization stay in Adapter, never Demo/Python. Demo may build
  carriers, hold buffers, invoke the SDK and display/copy results. Shared DTO types do not imply
  shared payload semantics; Node/Catalog ports are internal. Platform mocks live only in
  `include/platform_mock/`; framework entrypoints/helpers stay under `edgeflow/`.
- **Orchestration:** `PipelineValidator` alone derives data dependencies from explicit
  `inputs` / `outputs` bindings and combines optional `depends_on` ordering constraints.
  `Pipeline` consumes `ValidatedPipelinePlan` without reparsing/resorting. Request values use
  `AlgContext` and typed `BlackboardKey<T>` ports; session resources use `SessionContext`.
- **Capability Nodes:** common Nodes are neutral framework operations; custom Nodes are
  reusable domain algorithms organized by operation, not biz, in `src/custom_nodes/`.
  Both are request-stateless and use ordinary functions plus typed Specs, registered through
  `REGISTER_FUNCTION_NODE`; `AuthorNode` owns the `NodeBase` runtime and generated Definition. Custom algorithms need not be
  generalized. Common Nodes, Core, and Engine must not depend on custom implementations.
  Nodes use typed logical ports and `IModel` capabilities, never platform structs/conversion.
- **Model Execution:** Models own preprocessing/semantics; Backends own vendor runtime resources
  and neutral execution protocols. Register with `REGISTER_MODEL_WITH_DEFINITION` and
  `REGISTER_BACKEND_WITH_DEFINITION`. Vendor headers stay in the concrete Backend.
  Fixed-batch paths use `FixedBatchExecutor::Execute` for padding removal and `(req_id, sub_id)`
  provenance.

## Agent responsibilities

Roles are model- and provider-agnostic. Model selection, reasoning effort, sandbox settings,
and provider runtime routing belong only in provider configuration (Codex: `.codex/`).
The primary agent owns requirements, architecture/public contracts, non-mechanical implementation,
coordination, durable documentation, and final completion against the requested outcome.

- **Scout:** read-only exploration, call chains, impact analysis, and test discovery; no edits
  or architecture decisions. Use when unfamiliar scope justifies separate discovery.
- **Mechanical worker:** bounded low-risk edits from an already-decided plan. Return new
  architecture/public-contract/ownership/cross-layer decisions to the primary agent.
- **Test author:** independent focused behavior/contract tests; separate test-file ownership
  from production edits when useful. Do not change production code just to satisfy tests.
- **Verifier:** run relevant diagnostic checks and the single canonical gate defined in
  CONTRIBUTING. Report commands, results, and skips; return defects to source/test owners
  rather than silently fixing them.
- **Reviewer:** read-only independent review of high-risk Operator, cross-layer, Core/Pipeline,
  ownership/lifetime/concurrency, Model/Backend, or architectural design changes. Check correctness, boundaries,
  regressions, and whether tests prove the contract; routine low-risk edits need no reviewer.

Small edits may stay with the primary agent plus a Verifier. Add other agents only for concrete
work; do not split compilation and test execution into separate empty tasks. Give delegated
work a scope, owned files, and acceptance evidence; delegate only decided mechanical edits.
Avoid competing builds in one directory. Skills that change behavior include focused tests;
a handoff or first implementation is not completion. Follow CONTRIBUTING for iteration,
phase acceptance, the final gate, and honest reporting of blocked verification.

### Delegation context

All roles follow [Start with the requested scope](#start-with-the-requested-scope) and
[design and current contracts](CONTRIBUTING.md#3-design-and-current-contracts); provider role files reference, rather than redefine,
those rules. Pass only task-relevant decisions and evidence. Reuse supplied context while it
is valid for the target revision and scope; independent review still verifies required evidence.
Return concise findings, file/line or artifact references, and exact check results/skips.
Do not copy entire design discussions or logs into handoffs unless necessary to establish the result.

## Repository guardrails

Preserve unrelated user changes; no destructive Git operations or direct pushes to `main`.
Do not bundle third-party sources/binaries; dependencies stay pinned and verified in `cmake_ext/`.
The external workspace cannot access the company-internal SDK: do not request, infer, copy,
or commit its headers, libraries, models, configuration, or credentials. Prepare only neutral
integration seams; real SDK integration and target-hardware acceptance start only after the
complete project enters the authorized internal network. There, verify real public headers,
enum values, layouts, ownership, control and I/O conversion before target-hardware acceptance.
Keep platform mocks explicitly separate from that integration. Current verification scope and
deployment acceptance limits are described in [the verification guide](doc/VERIFIABLE_SELECTION.md).
