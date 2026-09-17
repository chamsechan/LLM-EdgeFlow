# Integration

Use this reference for new modalities, Operator structures, Converter behavior, business binding registration, or allowed Pipeline names.

Business input/output means the complete public Operator SDK request/response, including serialized
payload semantics. `InputConverter::Decode` and `OutputConverter::Encode` must implement that contract inside the SDK;
Demo/Python cannot perform the missing field selection or response assembly. Reusing a host struct
does not imply payload compatibility. Follow [the boundary and carrier distinction](../../../../doc/dev_guide/business_onboarding.md#输入输出以-operator-sdk-为边界).

Start with [business onboarding](../../../../doc/dev_guide/business_onboarding.md) to select the requested
integration path. Reuse the converters when the external contract is unchanged. Adding a production
binding to the current shared SDK requires matching input and output converters registered with `transport = "operator"`.
For new Operator host types, also register ValueType capacity, initialization and release.
Register ValueTypes and named single-object output allocators through
`adapter/operator_value_type.h`. Keep queue depth out of their callbacks. For multiple outputs
or config-selected nested payloads, follow the
[output allocation guide](../../../../doc/dev_guide/operator_output_allocation.md): each logical
slot selects its outer type, allocator and normalized parameters; map keys do not infer layout.
Keep configuration reading in Create-time Integration. `OperatorConfigResolver` validates
slot configurations, allocator and capacities. It is not a Pipeline Node and does not run per request.

1. Public Operator contract or new modality changes meet the RFC threshold in
   `CONTRIBUTING.md`. Map the external contract, ownership, cardinality, batch bounds, and
   failure behavior before implementation.
2. Operator public API lives in `include/edgeflow/operator/interface.h` and `types.h`. Platform mock types live in `include/platform_mock/operator_types.h` and `operator_data_types.h`; see that directory's README for the distinction from real company headers.
3. Preserve exported Operator functions and their exception barrier in `src/adapter/operator/operator_adapter.cpp`: `noexcept`, `try`, `catch (const std::exception&)`, and `catch (...)`.
4. Implement input conversion through `InputConverter` under `src/adapter/input/`, output conversion through `OutputConverter` under `src/adapter/output/`, and business binding through `IoBindingDefinition` under `src/adapter/biz/`. Register through `REGISTER_INPUT_CONVERTER`, `REGISTER_OUTPUT_CONVERTER`, and `REGISTER_IO_BINDING`.
5. Declare binding ingress/egress Blackboard ports and allowed runtime Pipeline names through its `BizExposureDefinition` and `IoBindingDefinition`. The `biz_name` in Pipeline JSON must match the binding.
6. Copy input data when the lifetime requires it, store request-scoped values in `AlgContext`, and pack output into leased pool slots only through the documented ownership contract.

Use `tests/contract/abi/test_cpp_operator_sdk.cpp`, `tests/contract/abi/test_operator_safety.cpp`, `tests/contract/abi/test_adapter_contract_security.cpp`, and existing modality converters as live templates. If the change also adds nodes, read `capability-nodes.md`; if it changes Core contract behavior, read `orchestration.md`.
