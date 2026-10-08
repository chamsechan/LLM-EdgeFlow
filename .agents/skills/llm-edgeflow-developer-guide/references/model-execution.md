# Model Execution

Use this reference for Model semantics/capabilities, a new Backend or neutral execution protocol,
vendor runtime integration, or batch scheduling behavior.

- First separate the need: Model owns preprocessing, input/output interpretation, and capability semantics; Backend owns vendor runtime loading/resources and implements a neutral protocol. Do not reintroduce a combined `*Engine` abstraction.
- Implement Models against `include/engine/model_interface.h` and neutral sessions from `backend_interface.h`. Put semantic implementations under `src/engine/models/<model>/` and register a complete `ModelDefinition` through `REGISTER_MODEL_WITH_DEFINITION`.
- Implement Backends through `IInferenceBackend`, keep vendor headers/resources under `src/engine/backends/<backend>/`, and register a complete `BackendDefinition` through `REGISTER_BACKEND_WITH_DEFINITION`.
- [The engine source list](../../../../src/engine/CMakeLists.txt) collects `.cpp` files under
  `src/engine/backends/` into `edgeflow_model_execution_backends_objects` and under
  `src/engine/models/` into `edgeflow_model_execution_objects`; runtime sources stay listed
  explicitly. Keep vendor include paths, `HAVE_*` definitions and imported-target compile requirements `PRIVATE`
  to the Backend object target. Pass final runtime dependencies through `$<LINK_ONLY:...>` as in
  [the root CMake configuration](../../../../CMakeLists.txt), so their compile requirements do not
  propagate to Models or upper layers.
- Definitions declare capability/protocol, concurrency, description, and `params`, a `ParameterSet`
  (`contracts/parameter_set.h`) holding every supported field/default/range. Models, Backends, Nodes and
  converters write parameters the same way in four parts: a `Params` struct, a `Parameters<Params>`
  declaration (`Field(...)` with default, range, enum and a Chinese description; cross-field rules in
  `Validate`), `def.params = ParamSpec();`, and `ctx.Params<Params>()` (Model) / `spec.Params<Params>()`
  (Backend) to read the values. Defaults, ranges and descriptions are written once, in the declaration.
  Catalog visibility follows registration without Web or skill edits.
- Parameters are validated once, in `ParameterSet::Parse`: field schema first, then field assignment, `Prepare`
  and `Validate` (pure; no resource loading or I/O). `PipelineValidator` parses for pre-flight diagnostics and
  [ModelRuntimeFactory](../../../../src/engine/runtime/model_runtime_factory.cpp) parses the model and backend
  groups once each before creating the Backend provider; invalid parameters fail before provider creation or
  `Load`. `Model::Create` and `Backend::Load` do not re-check ranges, enums, required fields or cross-field rules;
  they only check what needs loaded content (session type, batch policy, tensor shapes, whether files open).
  [VisionDocumentModel](../../../../src/engine/models/vision_document/vision_document_model.cpp) shows a
  `Validate` rule; [LlamaCppBackend](../../../../src/engine/backends/llama_cpp/llama_cpp_backend.cpp) shows a
  backend one.
- A `std::optional<T>` member declares a parameter that may be omitted (it cannot also declare `Required()` or
  `Default()`; the Catalog shows it as not required with no default). For facts the model file can really
  provide, such as a fixed ONNX tensor shape, resolve the final value with
  [ResolveFromModel](../../../../src/engine/models/common/from_model.h): a configured value must agree with
  the model, an omitted one uses the model's value, then a fallback, otherwise Create fails and asks for the
  parameter. [BgeEmbeddingModel](../../../../src/engine/models/bge_embedding/bge_embedding_model.cpp) uses it
  for `embedding_dim` and `max_length`.
- BackendLoadSpec requires an explicit execution protocol; runtime session checks still verify the actual protocol. Batch policy belongs to the session, not IModel.
- QwenCausalLmModel selects ChatML through its model type; there is no configurable template selector.
- Fixed-batch Model paths call `FixedBatchExecutor::Execute` so padding, dummy removal, and `(req_id, sub_id)` provenance remain consistent.
- Ordinary per-item Model paths use `FixedBatchExecutor::ExecuteItems` with a single input/output callback; it owns the loop, provenance and whole-batch rollback. Keep model-specific whole-batch prevalidation before this call, and preserve existing exception codes when migrating. It rejects nonempty fixed batches; do not replace tensor batching with repeated item calls.
- Validate model paths/configuration and translate exceptions into framework errors. Vendor types must not escape the concrete Backend.
- Keep loaded Model/Backend sessions session-scoped and lifecycle-safe. Test failed construction/load, protocol and capability mismatch, concurrency compatibility, shape/batch boundaries, padding, and provenance.

Use `src/engine/models/bge_embedding/`, `src/engine/models/qwen_causal_lm/`,
`src/engine/backends/onnxruntime/`, `src/engine/backends/llama_cpp/`,
`tests/unit/engine/test_model_backend_decoupling.cpp`, and `tests/unit/engine/test_batch_executor.cpp` as live templates.
