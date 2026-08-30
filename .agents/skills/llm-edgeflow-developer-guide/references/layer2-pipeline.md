# Layer 2: Pipeline, Validator, and Blackboard

Use this reference only for Core scheduling, lifecycle, validation, typed Blackboard contracts, or session resources. Configuration-only workflows belong to `pipeline-composer` and must not modify Core.

- `PipelineValidator` is the side-effect-free preflight used by CLI, Web, Skills, and `Pipeline::Build`. Add a rule once here; never create a UI approximation.
- Validation must happen before model loading or node initialization and return stable codes, JSON Pointer paths, related nodes/ports, suggestions, topological order, and wavefront layers where possible.
- Runtime parsing and all composition tools are fail-closed and require explicit `id + depends_on`; do not add an implicit sequential path or a compatibility converter.
- Detect registry conflicts, unknown fields/types/ranges, model references/capabilities, self/ordinary cycles, duplicate dependencies, missing producers, duplicate producers, Adapter ingress/egress closure, and parallel write/safety conflicts.
- Define reusable typed keys with `BlackboardKey<T>` and use the same Key in node code and port Definitions. Request data remains in `AlgContext`; shared immutable/model resources remain in `SessionContext`.
- Preserve the Pipeline state machine and one-shot build semantics. A failed preflight or materialization must leave the instance failed, not partially ready.
- Avoid unnecessary request-data copies and upward dependencies from Core into concrete Nodes, Models, or Backends.

Use `include/core/pipeline.h`, `src/core/pipeline.cpp`, `include/core/pipeline_validator.h`, `src/core/pipeline_validator.cpp`, `include/core/alg_context.h`, and `tests/test_pipeline_config.cpp` as current implementation references.
