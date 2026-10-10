# Orchestration

Use this reference only for Core scheduling, lifecycle, validation, typed Blackboard contracts, or session resources. Configuration-only workflows belong to `pipeline-composer` and must not modify Core.

- `PipelineValidator` is the side-effect-free preflight used by CLI, Web, Skills, before `Pipeline::BuildFromPlan`. Add a rule once here; never create a UI approximation.
- Validation must happen before model loading or node initialization and return stable codes, JSON Pointer paths, related nodes/ports, suggestions, topological order, and wavefront layers where possible.
- Pipeline consumes only `ValidatedPipelinePlan`; JSON parsing and field normalization belong to the Validator. There is one strict validation path, including test fixtures.
- Runtime parsing and all composition tools are fail-closed. Require explicit `name` and required input bindings; derive data dependencies from unique producers and merge optional `depends_on` ordering constraints only in the Validator. Array order never supplies dependencies; do not add a compatibility converter.
- Node JSON uses top-level `inputs` (`node.port`); missing optional inputs stay unconnected. Model capability comes from its Definition and model references are mandatory. `max_parallel_workers` (1–64, default 1) is the only concurrency setting. Reject unknown structural fields.
- Model entries use category `type`, instance `name`, `file`, optional `params`, and
  `backend: {type, params?}`. Select one implementation by category/protocol before parameter
  validation; use generic parameter diagnostics under `/params` or `/backend/params`.
  Reject unused models, except when unknown node types prevent reliable binding analysis.
  Core preserves resolved file values; Integration owns their path rules.
- Detect registry conflicts, unknown fields/types/ranges, model references/capabilities, self/ordinary cycles, duplicate dependencies, unknown node/port references, port type mismatches, duplicate input producers, output closure, and concurrency safety conflicts.
- Define reusable typed keys with `BlackboardKey<T>` and use the same Key in node code and port Definitions. Request data remains in `AlgContext`; shared immutable/model resources remain in `SessionContext`.
- Preserve the Pipeline state machine and one-shot build semantics. A failed preflight or materialization must leave the instance failed, not partially ready.
- Avoid unnecessary request-data copies and upward dependencies from Core into concrete Nodes, Models, or Backends.

Use `include/core/pipeline.h`, `src/core/pipeline.cpp`, `include/core/pipeline_validator.h`, `src/core/pipeline_validator.cpp`, `include/core/alg_context.h`, and `tests/unit/core/test_pipeline_config.cpp` as current implementation references.

Node entries use `type`, `name`, optional `params`, `inputs`, and `depends_on`. Source references
are `node.port` or `input.port`; `input` / `output` are reserved names. Only referenced outputs
appear in the plan. Input bindings carry the producer lifetime; `FollowLifetime(input)` propagates
it through an output, and `BindingFacts::InputLifetime` exposes it during parameter preparation.
