# Capability Nodes

Use this reference for production Node implementation. Start first-time LLM authors with the
[first Node exercise](../../../../doc/dev_guide/first_custom_node.md); consult the
[concept guide](../../../../doc/dev_guide/custom_node_concepts.md) for signatures and batch contracts.

1. Query the target build's `alg_pipeline_tool catalog` and `describe-node` before adding a capability.
2. Keep neutral operations in `src/common_nodes/` and domain algorithms in `src/custom_nodes/`,
   organized by operation. Common Nodes, Core and Engine must not depend on custom implementations.
   Platform conversion stays in Adapter. Follow CONTRIBUTING for design review criteria.
3. Every Node has the same structure: `Inputs`, optional `Params` and `Models`, one `Run` and a
   `Spec` built with `MakeNodeSpec`, registered with `REGISTER_FUNCTION_NODE`. `Run` takes
   `const Inputs&`, then `const Params&` and `const Models&` only when the Spec declares them, and
   finally `const SessionResources&` when it uses session caches. Per-item work inside `Run` uses
   `MapPayloads`, which keeps provenance and names the failing item.
   `NodeBase` is internal runtime infrastructure, not another business authoring choice.
4. Declare input views with `InputsOf` and typed members. `Required` requires a value; `Optional`
   permits an unconnected port; `OptionalValue` also permits a connected port without a request value
   when the algorithm owns that policy. Specify non-default flow using `PortFlow`.
5. Use `PreservedOutput` with an anchor for checked count/order/provenance. Use `ProducedBatch`
   or `OutputsOf` / `Produced` for derived or multiple outputs. Derived-flow correctness remains
   the algorithm's responsibility; declarations do not prove splitting or aggregation semantics.
   Complete preserved-output checks precede publication of any output.
6. Use typed `Parameters` / `Field` declarations for parameters, including defaults, bounds and semantic
   descriptions. Arrays use `std::vector<T>` and maps use `std::map<std::string, T>`; declare struct
   elements with `.Items(Parameters<Element>{...})`. `Range` / `Enum` constrain scalar leaves, and
   validation paths identify the array index or map key. `nlohmann::json` fields accept any non-null
   JSON value. Use `Validate` / `ValidateBindings` for semantic and connection rules. `Prepare`
   builds owned derived state, such as template tokens or compiled regex, before those checks.
7. Declare model dependencies with `ModelsOf` / `Model`; member types select `LlmCall`,
   `EmbeddingCall`, `AsrCall`, `OcrCall` or `RerankCall`. `Model("generator", "bind_model",
   &Models::generator)` takes three arguments; the framework generates its `models[].name`
   reference description from the member's capability. These facades handle empty batches,
   model diagnostics and alignment checks. Model-reference fields are required and have no default
   instance name. Propagate `NodeResult` failures without remapping shared
   errors to old node-specific codes. Keep domain failure codes where they describe actual algorithms.
   `ValidateModels` receives parsed parameters and bound models once during Create. Use it to
   reject unsupported explicit options, such as ASR language or a fixed LLM random seed.
   `system_prompt`, `random_seed` and `language` are call options; embedding normalization
   belongs to the shared model parameters. Domain post-processing remains in the node.
8. Keep request data local. A `Run` needing session resources explicitly accepts
   `const SessionResources&`; the facade exposes cache access and model revision queries, not arbitrary
   model lookup or request Blackboard access. TextEmbeddingNode is the compiled cache example.
   Use `GetOrCreateResult<T>` for a factory returning `NodeResult<T>`; the facade preserves failures
   for single-flight waiters without caching them. Resource keys and model revision remain explicit.
9. Use `WithControls` / `ReplaceFields` for parameter updates; the framework derives the payload
   schema from the field declarations. `ReplaceFields` requires at least one controlled field in
   its payload and replaces only the supplied fields. Arrays and maps are replaced as complete values; field
   updates rerun `Prepare` and `Validate` before publication. The framework serializes updates,
   retains the old snapshot on failure and reads one snapshot per request. TextTemplateNode and
   TextRuleMatchNode are production examples.
   Follow the [Control guide](../../../../doc/dev_guide/first_control.md) for wire schema and delivery.
   Rebuild derived state in `Prepare` for both initialization and updates. All supported parameter
   types can be controlled; model-reference fields cannot be controlled.
10. Declare category and description in the Spec; Nodes are not restricted to particular businesses.
    Keep the default conservative parallel safety for sequential use; explicitly establish
    `.ParallelSafe(true)` only when making the implementation available to parallel graphs.
    Generated Definition is the only Catalog source; do not maintain a second UI registry.
    Initialization consumes a ValidatedNodePlan; do not call PipelineValidator inside a Node.

When generation options are a Node's only parameters, use `GenerateParameters()` from
[generate_parameters.h](../../../../include/nodes/generate_parameters.h) and pass the `GenerateOptions`
received by `Run` to `LlmCall::Generate`, as LlmGenerateNode and the LLM starter do. All callers share
`GenerateOptions` defaults, including `max_tokens = 128`. With additional fields, put a
`GenerateOptions` member in `Params` and use
`Parameters<Params>{Field(...)}.Include(&Params::generation, GenerateParameters())`, as
[PromptGuidedLlmNode](../../../../src/custom_nodes/prompt_guided_llm_node.cpp) does. Included fields
remain flat in JSON and duplicate names fail when building the declaration. The included group's
`Prepare` / `Validate` run before those of the outer group. Generation fields are ordinary typed
bindings, so `WithControls` can select them alongside the Node's own fields.

Use existing production implementations and matching `tests/unit/nodes/test_*_node.cpp` suites.
Add focused behavior and contract coverage for changed configuration, missing values, output provenance,
model failures, Control rollback/concurrency, cache behavior and Catalog/composition as applicable.
The authoring boundary writes returned failures to request diagnostics. Do not put Context handling,
manual port binding or hand-built Definitions back into ordinary business functions.

Use [batch helpers](../../../../doc/dev_guide/custom_node_concepts.md#复杂算法仍按普通-c-函数组织)
only where their documented join/group/split contracts fit. Do not generalize a domain algorithm merely
to fit a helper. The shared scaffold `--kind model -m asr` uses the same contract as other capabilities.
