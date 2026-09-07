# LLM-EdgeFlow Agent Governance

This file is the single source of truth for current architecture constraints and task routing.
The shared development lifecycle is defined in [CONTRIBUTING.md](CONTRIBUTING.md); do not
duplicate it in agent-specific instruction files.

The primary users are LLM solution developers: compose Pipelines, implement or reuse business
Nodes, adapt platform I/O, and verify through the shared Demo. Keep ordinary solution work in
those extension points; involve Core, Model or Backend changes only for a demonstrated gap.

## Current architecture invariants

Dependencies flow downward only:

```text
Layer 1  C ABI / Operator / Biz adapters
    ↓
Layer 2  Pipeline / Validator / Catalog / Blackboard / Session
    ↓
Layer 3  Stateless capability Nodes
    ↓
Layer 4  Model semantics / neutral execution protocols / Backends
```

- **Layer 1** — `include/company_alg_interface.h`, `include/operator/`,
  `include/adapter/`, and `src/adapter/`. Public C headers remain C11-only. All six exported
  `Alg_*` functions keep `noexcept`, `catch (const std::exception&)`, and `catch (...)`
  barriers. Biz-specific conversion belongs in registered `IBizAdapter` and Operator bridge
  implementations, not in central dispatch switches or lower layers.
- **Layer 2** — `include/core/` and `src/core/`. `PipelineValidator` is the single validation
  and planning implementation. Runtime Pipeline documents use explicit `id` and `depends_on`;
  `Pipeline` consumes `ValidatedPipelinePlan` without reparsing or resorting. Request values
  live in `AlgContext` behind typed ports/`BlackboardKey<T>`; session resources live in
  `SessionContext`.
- **Layer 3** — `src/common_nodes/`, `src/custom_nodes/`, and `include/nodes/`. Common Nodes
  provide framework-maintained, business-neutral operations; custom Nodes contain user-defined
  domain algorithms and can be reused across Pipelines. Keep custom node files organized by
  operation in one directory, not by business. Both are request-stateless, inherit `NodeBase`
  or its shallow support classes, and register constructor plus `NodeDefinition` through
  `REGISTER_NODE_WITH_DEFINITION`. Reuse Catalog operations before adding code; a domain
  algorithm need not be generalized to enter `custom_nodes`. Common Nodes, Core and Engine
  must not depend on custom implementations. All Nodes use typed logical ports and model
  capabilities; platform structs and conversion remain in Layer 1. Follow `CONTRIBUTING.md`
  for RFC thresholds and [custom Node onboarding](src/custom_nodes/README.md) for source layout.
- **Layer 4** — `include/engine/` and `src/engine/`. Nodes depend on typed `IModel`
  capabilities. Models own preprocessing/model semantics and register through
  `REGISTER_MODEL_WITH_DEFINITION`; Backends own vendor runtime resources, implement neutral
  execution protocols, and register through `REGISTER_BACKEND_WITH_DEFINITION`. Vendor headers
  stay under the concrete Backend. Fixed-batch model paths use `FixedBatchExecutor::Execute` to
  preserve padding removal and `(req_id, sub_id)` provenance.

Do not infer available nodes, ports, models, backends, biz contracts, or configuration fields
from prose. Query `alg_pipeline_tool`; registrations and Definitions are the executable catalog.

## Task routing

- Solution configuration using existing capabilities and biz contracts, including Pipeline JSON,
  necessary `.conf` files and optional Demo Profiles: read and follow
  [pipeline-composer](.agents/skills/pipeline-composer/SKILL.md). Reuse registered Nodes; route
  capability gaps to implementation before writing C++.
- C ABI/Adapter, Core/Pipeline, Node, Model, or Backend implementation: read and follow
  [llm-edgeflow-developer-guide](.agents/skills/llm-edgeflow-developer-guide/SKILL.md), loading
  only affected-layer references. New platform structures and Demo data conversion belong here;
  preserve the current SDK's Adapter/Operator bridge registry completeness when adding a biz.
- Upload, PR, or merge requested by the user: read and follow
  [github-branch-merge](.agents/skills/github-branch-merge/SKILL.md). Never upload or merge from
  an ordinary implementation request.
- RFC decisions and status: follow [the RFC index](doc/rfcs/README.md) and
  [template](doc/rfcs/RFC_TEMPLATE.md).

## Repository guardrails

- Preserve unrelated user changes. Do not use destructive Git operations or push directly to
  `main`.
- Do not bundle third-party source or binaries. Dependency declarations remain pinned and
  verified through `cmake/`.
- The current external workspace cannot access the company-internal SDK. Do not request, infer,
  copy, or commit its headers, libraries, models, configuration, or credentials here. Prepare
  only vendor-neutral migration and integration seams; actual SDK integration and target-hardware
  acceptance begin only after the complete project moves into the authorized internal network.
  Follow [RFC-0029](doc/rfcs/0029-external-readiness-and-intranet-sdk-migration.md).
- Add or update the smallest tests that prove changed behavior; do not require a new executable
  when an existing focused suite is the correct home.
- `./scripts/run_all_tests.sh` is the canonical pre-delivery local gate. It already checks shell
  syntax, formatting, Git whitespace, configures/builds the complete default backend set, and
  runs all CTest tests. Do not routinely precede or follow it with duplicate full gates.
- Update `doc/CHANGELOG.md` only for user-visible or architectural changes. Keep README focused on
  the current product and navigation.
