# Capability Nodes

Use this reference for a new or modified `INode` implementation.

For a first custom LLM Node using an existing model capability, start with the
[two-function exercise](../../../../doc/dev_guide/first_custom_node.md) and its compiled starter.
Use the [concept guide](../../../../doc/dev_guide/custom_node_concepts.md) as needed; multi-input,
filtering or aggregation logic needs the full port and provenance contracts below.

1. Query `alg_pipeline_tool catalog` and `describe-node` first. Add a node only when existing registered capabilities cannot close the required contract.
2. Put framework-maintained neutral operations in `src/common_nodes/` and user-defined domain algorithms in `src/custom_nodes/`. Keep custom files organized by operation in the shared directory; they can be reused across businesses and need not be generalized for admission. Both use the same base classes and registration path. Follow `CONTRIBUTING.md` for RFC thresholds and [custom Node onboarding](../../../../src/custom_nodes/README.md) for authoring; platform conversion remains in Adapter, and Common Nodes/Core/Engine must not depend on custom implementations.
3. Inherit `NodeBase` (or `ModelBoundNode`, `TraceableUnaryInferenceNode`); keep per-request state exclusively in `AlgContext`. Members may hold immutable configuration or safe shared handles, and the Definition must truthfully declare parallel safety.
4. Declare inputs/outputs with the same `BlackboardKey<T>` objects used by `ProcessNode`. Never guess or duplicate key strings with inconsistent types.
5. Provide a complete `NodeDefinition`: category, description, typed ports, configuration fields/defaults/ranges, model capability/reference field where relevant, biz applicability, override policy, and parallel safety.
6. Register constructor and Definition together. A registered production node must appear automatically in `alg_pipeline_tool catalog`; never modify a Web list, skill table, or hand-maintained secondary Catalog.
7. Validate configuration in `Init` as a defensive runtime boundary even though static validation runs first. Return errors; do not throw across framework boundaries.
8. Add focused GoogleTest coverage for the affected configuration, port failures, outputs, provenance, concurrency declaration, Catalog visibility, and valid composition. Extend an existing suite when it already owns the contract.

Use existing implementations in `src/common_nodes/`, the `src/custom_nodes/` authoring guide, and matching
`tests/unit/nodes/test_*_node.cpp` suites as current templates. Use
`tests/integration/pipeline/test_pipeline_catalog_validator.cpp` for Catalog/Validator integration;
do not copy implementations into documentation.

RFC-0044 config contract: reuse `ValidateAndNormalizeFields` from `contracts` in defensive
initialization, using the same field list as the Definition. `ModelBoundNode` already does this
before model binding. Keep cross-field semantic checks in a shared local helper; do not call
PipelineValidator from a Node. Report processing failures through `Fail` / `Require`.
