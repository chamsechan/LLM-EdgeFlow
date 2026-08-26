# Layer 3: Business and Common Nodes

Use this reference for a new or modified `INode` implementation.

1. Query `alg_pipeline_tool catalog` and `describe-node` first. Add a node only when existing registered capabilities cannot close the required contract.
2. Put a node in `src/common_nodes/` only when it contains no business knowledge, has reusable typed ports, and at least two business tests demonstrate reuse. Otherwise keep it under `src/business/<business>/`.
3. Inherit `NodeBase` (or `ModelBoundNode`, `TraceableUnaryInferenceNode`); keep per-request state exclusively in `AlgContext`. Members may hold immutable configuration or safe shared handles, and the Definition must truthfully declare parallel safety.
4. Declare inputs/outputs with the same `BlackboardKey<T>` objects used by `ProcessNode`. Never guess or duplicate key strings with inconsistent types.
5. Provide a complete `NodeDefinition`: category, description, typed ports, configuration fields/defaults/ranges, model capability/reference field, business applicability, override policy, and parallel safety.
6. Register constructor and Definition together. A registered production node must appear automatically in `alg_pipeline_tool catalog`; never modify a Web list, skill table, or hand-maintained secondary Catalog.
7. Validate configuration in `Init` as a defensive runtime boundary even though static validation runs first. Return errors; do not throw across framework boundaries.
8. Add focused GoogleTest coverage for configuration, missing/type-wrong Blackboard values, outputs, provenance, concurrency declaration, Catalog visibility, and at least one valid Pipeline composition.

Use existing implementations in `src/business/`, `src/common_nodes/`, `tests/test_rerank_refine_node.cpp`, and `tests/test_pipeline_studio.cpp` as current templates instead of copying code into documentation.
