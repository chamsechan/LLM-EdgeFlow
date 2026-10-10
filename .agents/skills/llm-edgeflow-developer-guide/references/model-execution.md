# Model Execution

Use this reference for Model semantics/capabilities, a new Backend or neutral execution protocol,
vendor runtime integration, or batch scheduling behavior.

- First separate the need: Model owns preprocessing, input/output interpretation, and capability semantics; Backend owns vendor runtime loading/resources and implements a neutral protocol. Do not reintroduce a combined `*Engine` abstraction.
- Implement Models against `include/engine/model_interface.h` and neutral sessions from `backend_interface.h`. Put semantic implementations under `src/engine/models/<model>/` and register a complete `ModelDefinition` through `REGISTER_MODEL_WITH_DEFINITION`.
- Implement Backends through `IInferenceBackend`, keep vendor headers/resources under `src/engine/backends/<backend>/`, and register a complete `BackendDefinition` through `REGISTER_BACKEND_WITH_DEFINITION`.
- [The engine source list](../../../../src/engine/CMakeLists.txt) collects `.cpp` files under
  `src/engine/backends/` into `edgeflow_model_execution_backends_objects` and under
  the remaining `src/engine/` implementations into `edgeflow_model_execution_objects`, including
  Models, runtime and shared helpers. Keep vendor include paths, `HAVE_*` definitions and imported-target compile requirements `PRIVATE`
  to the Backend object target. Pass final runtime dependencies through `$<LINK_ONLY:...>` as in
  [the root CMake configuration](../../../../CMakeLists.txt), so their compile requirements do not
  propagate to Models or upper layers.
- Definitions declare capability/protocol, concurrency, description, and every supported config field/default/range. PipelineValidator validates these typed fields before planning; a concrete Backend may additionally consume one explicitly declared vendor run-config field when its SDK owns that configuration format. Catalog visibility follows registration without Web or skill edits.
- Author parameters with a `Params` struct, `ParamSpec()` returning `Parameters<Params>`,
  `def.params = ParamSpec()`, and `ctx.Params<Params>()` / `spec.Params<Params>()` for consumption.
  Put pure parameter rules in `ParamSpec().Validate`, with no resource loading or I/O.
  `PipelineValidator` checks fields and parameter semantics while planning;
  [ModelRuntimeFactory](../../../../src/engine/runtime/model_runtime_factory.cpp) parses each group
  once before Backend provider creation or `Load`. `Create` and `Load` consume immutable values
  and check sessions/resources without repeating pure parameter validation. Direct-call tests
  first use the Definition's `params.Parse`.
- Optional scalar members have no default and cannot use `Required()` or `Default()`.
  [ResolveFromModel](../../../../src/engine/models/common/from_model.h) selects a configured value,
  fixed model fact, or fallback; conflicting configured/fixed values fail. BGE reads fixed output
  dimensions and input sequence length, with 512 as the sequence fallback. A dynamic embedding
  dimension must be configured. Catalog shows the declaration; creation logs the selected value.
- BackendLoadSpec requires an explicit execution protocol; runtime session checks still verify the actual protocol. Batch policy belongs to the session, not IModel.
- Model entries use `{type, name, file, params?, backend: {type, params?}}`. Category and
  Backend protocol select one `impl_name`; GlobalInit audits uniqueness and Core rejects ambiguity.
  Fixture models require `kFixture` and match only their declared `fixture_backends`.
  Declare file parameters with `.File()`. Integration resolves them against the Pipeline JSON
  directory before Core validation; Models/Backends receive resolved paths and check resources
  when opening them. Kite projector paths use the run-config directory.
- LLM system prompts/seeds and ASR language are per-call options. Nodes check explicit unsupported
  requirements once in `ValidateModels`. Vector normalization and pooling are model parameters.
- QwenCausalLmModel selects ChatML through its model type; there is no configurable template selector.
- Fixed-batch Model paths call `FixedBatchExecutor::Execute` so padding, dummy removal, and `(req_id, sub_id)` provenance remain consistent.
- Ordinary per-item Model paths use `FixedBatchExecutor::ExecuteItems` with a single input/output callback; it owns the loop, provenance and whole-batch rollback. Keep model-specific whole-batch prevalidation before this call, and preserve existing exception codes when migrating. It rejects nonempty fixed batches; do not replace tensor batching with repeated item calls.
- Validate resource contents/configuration and translate exceptions into framework errors. Vendor types must not escape the concrete Backend.
- Keep loaded Model/Backend sessions session-scoped and lifecycle-safe. Test failed construction/load, protocol and capability mismatch, concurrency compatibility, shape/batch boundaries, padding, and provenance.

Use `src/engine/models/bge_embedding/`, `src/engine/models/qwen_causal_lm/`,
`src/engine/backends/onnxruntime/`, `src/engine/backends/llama_cpp/`,
`tests/unit/engine/test_model_backend_decoupling.cpp`, and `tests/unit/engine/test_batch_executor.cpp` as live templates.
