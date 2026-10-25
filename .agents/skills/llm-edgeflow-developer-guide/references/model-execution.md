# Model Execution

Use this reference for Model semantics, Backend resources or neutral execution protocols.
The [Model/Backend extension guide](../../../../doc/developer_guide.md#4-模型执行层如何新增模型语义或推理-backend)
owns parameter preparation, identity/registration, model selection, batch execution, Tensor ownership,
call options and generation behavior. Build and asset acceptance limits belong to
[verification and selection](../../../../doc/VERIFIABLE_SELECTION.md).

Implementation checklist:

1. Assign semantics and preprocessing to Model, vendor loading/resources to Backend.
   Reuse an existing neutral Session protocol when it fits; keep vendor types out of Models and upper layers.
2. Register complete Definitions and implement the declared capability/protocol and concurrency.
   Keep pure parameter rules in `ParamSpec`; Create/Load check real resources and sessions.
   Direct-call tests must obtain parsed parameters through the Definition.
3. Follow [the engine targets](../../../../src/engine/CMakeLists.txt): vendor include paths,
   `HAVE_*` definitions and imported compile requirements stay `PRIVATE` to the Backend object target.
   Final runtime dependencies use `$<LINK_ONLY:...>` so compile requirements do not propagate upward.
4. Use `FixedBatchExecutor::Execute` for fixed batches and `ExecuteItems` for ordinary item loops.
   Preserve padding removal, provenance, business prevalidation and whole-batch failure cleanup.
5. Keep shared Tensor buffers alive through execution and decoding. Translate vendor failures
   at the Backend boundary, retaining the cause in framework diagnostics.
6. Test construction/load failure, actual session protocol and capability, shapes, batch boundaries,
   concurrency and lifetime as applicable. Real execution requires the selected build and assets;
   passing fixture tests does not establish runtime or hardware acceptance.

Current examples are [BGE](../../../../src/engine/models/bge_embedding/),
[Qwen](../../../../src/engine/models/qwen_causal_lm/),
[ONNX Runtime](../../../../src/engine/backends/onnxruntime/) and
[llama.cpp](../../../../src/engine/backends/llama_cpp/), with tests under `tests/unit/engine/`.
Use the [Model skill](../../edgeflow-model-developer/SKILL.md) or
[Backend skill](../../edgeflow-backend-developer/SKILL.md) and finish with [verification](verification.md).
