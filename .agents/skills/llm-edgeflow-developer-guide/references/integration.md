# Integration

Use this reference for new modalities, Operator structures, Converter behavior, business binding registration, or allowed Pipeline names.

Business input/output means the complete public Operator SDK request/response, including serialized
payload semantics. `InputConverterDefinition::decode_fn` and `OutputConverterDefinition::encode_fn`
must implement that contract inside the SDK;
Demo/Python cannot perform the missing field selection or response assembly. Reusing a host struct
does not imply payload compatibility. Follow [the boundary and carrier distinction](../../../../doc/dev_guide/business_onboarding.md#输入输出以-operator-接口为边界).

Start with [business onboarding](../../../../doc/dev_guide/business_onboarding.md) to select the requested
integration path. Reuse the converters when the external contract is unchanged. Adding a production
binding to the current shared SDK requires matching input and output converters and an explicit IoBinding registration.
For new Operator host types, also register ValueType capacity, initialization and release.
Register ValueTypes and named single-object output allocators through
`adapter/operator_value_type.h`. Keep queue depth out of their callbacks. For multiple outputs
or config-selected nested payloads, follow the
[output allocation guide](../../../../doc/dev_guide/operator_output_allocation.md): slot Definitions determine the outer type; deployment only overrides allocator and parameters. Required slots use registered defaults; optional slots are enabled explicitly. Map keys do not infer layout.
Keep configuration reading in Create-time Integration. `OperatorConfigResolver` validates
slot configurations, allocator and capacities. It is not a Pipeline Node and does not run per request.

1. Public Operator contract or new modality changes meet the design review criteria in
   `CONTRIBUTING.md`. Map the external contract, ownership, cardinality, batch bounds, and
   failure behavior before implementation.
2. Operator public API lives in `include/edgeflow/operator/interface.h`; `types.h` forwards platform data structures. Platform mock interaction types live in `include/platform_mock/operator_types.h`, and payload structures in `operator_data_types.h`; see that directory's README for the distinction from real company headers.
3. Preserve exported Operator functions and their exception barrier in `src/adapter/operator/operator_adapter.cpp`: `noexcept`, `try`, `catch (const std::exception&)`, and `catch (...)`.
4. Implement ordinary `DecodeInputFn` callbacks in `InputConverterDefinition` under `src/adapter/input/`, `EncodeOutputFn` callbacks in `OutputConverterDefinition` under `src/adapter/output/`, and business binding through `IoBindingDefinition` under `src/adapter/biz/`. Register through `REGISTER_INPUT_CONVERTER`, `REGISTER_OUTPUT_CONVERTER`, and `REGISTER_IO_BINDING`.
5. Register `BizDefinition` with `PipelineCatalog::RegisterBizDefinition` to declare `biz_name` and complete ingress/egress Blackboard ports. `IoBindingDefinition` selects converters and defaults to the standard batch bound of 64; converter logical port names are the biz Blackboard keys. External Pipeline JSON requires `deployment.io.io_binding` and rejects root `biz_name`; Integration derives the internal business boundary from the selected registration. Demo resolves its runner through the SDK configuration query; neither CLI nor Profile accepts a business selector. Each biz registers exactly one binding, so the biz identifies one external contract; a second binding for the same biz is a registry conflict that fails SDK initialization.
6. Copy input data when the lifetime requires it, store request-scoped values in `AlgContext`, and pack output into leased pool slots only through the documented ownership contract.

Bindings default to the framework standard batch bound of 64; override
`IoBindingDefinition::max_batch_size` only when measurements require a smaller bound. Converters declare a limit only
when they have one of their own, and zero adds no bound. The effective limit is the smallest
positive value among the binding and its converters, and a binding where all three are zero fails
the registry audit and [deployment preparation](../../../../src/adapter/deployment_preparation.cpp).
[Operator creation](../../../../src/adapter/operator/operator_adapter.cpp) further caps the effective
Process batch limit at the output pool depth; a larger binding limit cannot relax another limit.

For one required host slot, one business payload stream and one payload/result per request,
use `DecodeRequestRows` / `EncodeResultRows`
from `converter_authoring.h`. Business callbacks handle one owned payload or one borrowed output row;
helpers own looping, provenance, request IDs and diagnostic location. `OutputStringWriter`
uses actual pool capacities and explicit string lengths. Do not retain its borrowed view or pointers.
Independent metadata streams, multi-slot, expanded and aggregated conversions keep their explicit
algorithms and existing lower-level helpers; a single external slot alone does not imply a single stream.

Use `tests/contract/abi/test_cpp_operator_sdk.cpp`, `tests/contract/abi/test_operator_safety.cpp`, `tests/contract/abi/test_adapter_contract_security.cpp`, and existing modality converters as live templates. If the change also adds nodes, read `capability-nodes.md`; if it changes Core contract behavior, read `orchestration.md`.
