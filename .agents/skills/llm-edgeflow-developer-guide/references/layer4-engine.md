# Layer 4: Models and Inference Backends

Use this reference for Model semantics/capabilities, a new Backend or neutral execution protocol,
vendor runtime integration, or batch scheduling behavior.

- First separate the need: Model owns preprocessing, input/output interpretation, and capability semantics; Backend owns vendor runtime loading/resources and implements a neutral protocol. Do not reintroduce a combined `*Engine` abstraction.
- Implement Models against `include/engine/model_interface.h` and neutral sessions from `backend_interface.h`. Put semantic implementations under `src/engine/models/<model>/` and register a complete `ModelDefinition` through `REGISTER_MODEL_WITH_DEFINITION`.
- Implement Backends through `IInferenceBackend`, keep vendor headers/resources under `src/engine/backends/<backend>/`, and register a complete `BackendDefinition` through `REGISTER_BACKEND_WITH_DEFINITION`.
- Definitions declare capability/protocol, concurrency, description, and every supported config field/default/range. PipelineValidator validates these typed fields before planning; a concrete Backend may additionally consume one explicitly declared vendor run-config field when its SDK owns that configuration format. Catalog visibility follows registration without Web or skill edits.
- Fixed-batch Model paths call `FixedBatchExecutor::Execute` so padding, dummy removal, and `(req_id, sub_id)` provenance remain consistent.
- Validate model paths/configuration and translate exceptions into framework errors. Vendor types must not escape the concrete Backend.
- Keep loaded Model/Backend sessions session-scoped and lifecycle-safe. Test failed construction/load, protocol and capability mismatch, concurrency compatibility, shape/batch boundaries, padding, and provenance.

Use `src/engine/models/bge_embedding/`, `src/engine/models/qwen_causal_lm/`,
`src/engine/backends/onnxruntime/`, `src/engine/backends/llama_cpp/`,
`tests/test_model_backend_decoupling.cpp`, and `tests/test_batch_executor.cpp` as live templates.
