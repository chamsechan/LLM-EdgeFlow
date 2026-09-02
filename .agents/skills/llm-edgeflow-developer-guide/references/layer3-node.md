# Layer 3: Capability Nodes

Use this reference for a new or modified `INode` implementation.

1. Query `alg_pipeline_tool catalog` and `describe-node` first. Add a node only when existing registered capabilities cannot close the required contract.
2. Put production Nodes in `src/common_nodes/` when they express one reusable operation through typed ports. Business behavior belongs in Pipeline composition and Adapter boundaries. A truly non-configurable domain Node requires an RFC that proves common-node composition would violate semantics, atomicity, or performance; do not invent a directory or base class before that decision is accepted.
3. Inherit `NodeBase` (or `ModelBoundNode`, `TraceableUnaryInferenceNode`); keep per-request state exclusively in `AlgContext`. Members may hold immutable configuration or safe shared handles, and the Definition must truthfully declare parallel safety.
4. Declare inputs/outputs with the same `BlackboardKey<T>` objects used by `ProcessNode`. Never guess or duplicate key strings with inconsistent types.
5. Provide a complete `NodeDefinition`: category, description, typed ports, configuration fields/defaults/ranges, model capability/reference field where relevant, biz applicability, override policy, and parallel safety.
6. Register constructor and Definition together. A registered production node must appear automatically in `alg_pipeline_tool catalog`; never modify a Web list, skill table, or hand-maintained secondary Catalog.
7. Validate configuration in `Init` as a defensive runtime boundary even though static validation runs first. Return errors; do not throw across framework boundaries.
8. Add focused GoogleTest coverage for the affected configuration, port failures, outputs, provenance, concurrency declaration, Catalog visibility, and valid composition. Extend an existing suite when it already owns the contract.

Use existing implementations in `src/common_nodes/` and their matching
`tests/unit/nodes/test_*_node.cpp` suites as current templates. Use
`tests/integration/pipeline/test_pipeline_catalog_validator.cpp` for Catalog/Validator integration;
do not copy implementations into documentation.
