# Layer 4: Inference Engines

Use this reference for a new backend, hardware integration, engine capability, or batch scheduling behavior.

- Implement the narrow interface in `include/engine/engine_interface.h`; keep vendor headers and resources inside `src/engine/<backend>/`.
- Provide an `EngineDefinition` with engine type, semantic capability, description, and every supported configuration field/default/range.
- Register constructor and Definition together. Catalog visibility must follow registration without edits to Web or skills.
- Any fixed-batch inference path must call `FixedBatchExecutor::Execute` so padding, dummy removal, and `(req_id, sub_id)` provenance are handled consistently.
- Validate model paths/configuration and translate backend exceptions into framework errors. Do not expose vendor types to Layers 1–3.
- Keep device/model resources session-scoped and lifecycle-safe. Test failed construction/load, repeated lifecycle transitions, shape/batch boundaries, padding, provenance, and capability mismatch.

Use `src/engine/mock_npu/`, conditional implementations under `src/engine/onnx/` and `src/engine/llama_cpp/`, `tests/test_batch_executor.cpp`, and `tests/test_engine_fault_tolerance_and_lifecycle.cpp` as live templates.
