# Capability Nodes

Use this reference for production Node implementation. First-time authors use the
[first Node exercise](../../../../doc/dev_guide/first_custom_node.md).
The [Node concept guide](../../../../doc/dev_guide/custom_node_concepts.md) owns signatures,
ports, parameters, model calls, batch provenance, caches and concurrency contracts.
Source collection, scaffold commands and focused tests belong to the
[source directory guide](../../../../src/custom_nodes/README.md).

Implementation checklist:

1. Query the target build's Catalog and `describe-node` before adding a capability.
2. Keep neutral operations in `src/common_nodes/` and domain algorithms in `src/custom_nodes/`.
   Common Nodes, Core and Engine must not depend on custom implementations. Platform conversion
   stays in Adapter; domain algorithms need not be generalized.
3. Use ordinary functions with `MakeNodeSpec` and `REGISTER_FUNCTION_NODE`. Follow the
   [signature table](../../../../doc/dev_guide/custom_node_concepts.md#结构与签名速查).
   Do not hand-build Definitions, bind Blackboard ports or call PipelineValidator in the algorithm.
4. Choose input optionality and output flow explicitly. Preserved outputs require count/order/
   provenance checks before any publication; derived flows need algorithm tests. Use batch helpers
   only when their documented split/join/group contracts fit, including borrowed-view lifetimes.
5. Declare fields once and build owned derived state in `Prepare`. Use the
   [parameter rules](../../../../doc/dev_guide/custom_node_concepts.md#3-definition让连线工具和运行器看懂你的操作)
   for containers, shared generation fields and semantic validation. Control reuses these rules;
   follow the [Control guide](../../../../doc/dev_guide/first_control.md) for delivery and rollback.
6. Bind typed model capabilities; propagate their failures and keep request values local.
   [Model binding and cache rules](../../../../doc/dev_guide/custom_node_concepts.md#2-模型绑定拿到一个已经准备好的模型能力)
   distinguish session resources from per-call options. A cache key must include all semantic inputs.
7. Retain conservative sequential defaults unless actual shared resources support parallel use.
   Check the Node and bound models before enabling parallel graphs.
8. Extend the existing Node suite for changed behavior: inputs, outputs, failure diagnostics,
   provenance, model failures, cache isolation or Control rollback/concurrency as applicable.
   Validate and run the edited Pipeline with the target build; use [verification](verification.md).
