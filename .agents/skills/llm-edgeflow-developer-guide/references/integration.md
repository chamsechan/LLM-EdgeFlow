# Integration

Use this reference for new modalities, Operator structures, Converter behavior, converter registration and parameters, or allowed Pipeline names.

Business input/output means the complete public Operator SDK request/response, including serialized
payload semantics. `InputConverterDefinition::decode_fn` and `OutputConverterDefinition::encode_fn`
must implement that contract inside the SDK;
Demo/Python cannot perform the missing field selection or response assembly. Reusing a host struct
does not imply payload compatibility. Follow [the boundary and carrier distinction](../../../../doc/dev_guide/business_onboarding.md#输入输出以-operator-接口为边界).

Start with [business onboarding](../../../../doc/dev_guide/business_onboarding.md) to select the requested
integration path. Reuse the converters when the external contract is unchanged. Adding a production
business to the current shared SDK requires registering matching input and output converters for the (struct, business) pair; the Pipeline `io` selects them.
For a new platform host type, add its struct, its traits in `include/adapter/io_converter.h` and
one ValueType entry (capacity, initialization and release) in `operator_builtin_value_types.cpp`;
the three stay one-to-one. Add a new nested layout for an existing type as a named allocator in its
own `.cpp` (`REGISTER_OPERATOR_OUTPUT_ALLOCATOR` from `adapter/operator_value_type.h`), not as a branch
in the existing implementation; use `params` for values tunable within one implementation. A registration's slot declaration fixes its named layout, layout parameters and metadata, so a converter always matches the layout it writes. Keep queue
depth out of ValueType and allocator callbacks. For multiple outputs
or config-selected nested payloads, follow the
[output allocation guide](../../../../doc/dev_guide/operator_output_allocation.md): slot declarations determine the outer type, layout and metadata; deployment only overrides the converter's size and behavior parameters in `io` (`<field>_max_bytes`, checked against the platform maximum). Optional slots are declared in the registration. Map keys do not infer layout.
Keep configuration reading in Create-time Integration. Deployment preparation validates
the `io` entries, converter parameters and capacities. It is not a Pipeline Node and does not run per request.

1. Public Operator contract or new modality changes meet the design review criteria in
   `CONTRIBUTING.md`. Map the external contract, ownership, cardinality, batch bounds, and
   failure behavior before implementation.
2. Operator public API lives in `include/edgeflow/operator/interface.h`; `types.h` forwards platform data structures. Platform mock interaction types live in `include/platform_mock/operator_types.h`, and payload structures in `operator_data_types.h`; see that directory's README for the distinction from real company headers.
3. Preserve exported Operator functions and their exception barrier in `src/adapter/operator/operator_adapter.cpp`: `noexcept`, `try`, `catch (const std::exception&)`, and `catch (...)`.
4. Implement ordinary `DecodeInputFn` callbacks in `InputConverterDefinition` under `src/adapter/input/` and `EncodeOutputFn` callbacks in `OutputConverterDefinition` under `src/adapter/output/`. Register through `REGISTER_INPUT_CONVERTER` and `REGISTER_OUTPUT_CONVERTER`.
5. Each registration is one (struct, business) pair: `type` is the host struct (map key suffix such as `doc_in`), `name` the business (such as `doc_qa`), and `service_type` the value of the struct's `service_type` member for that business (omitted for the reserved `common` name and for structs without the member). The framework checks `service_type` before each request reaches the callback and sets it on output structs. The converter's logical port names are the Blackboard keys. External Pipeline JSON requires root `io` with `input` and `output` arrays whose entries select registrations by (`type`, `name`) and may override the converter's `params`; Integration builds the internal IO boundary from the selected registrations, and Core has no business concept. Demo resolves its runner through the SDK configuration query; neither CLI nor Profile accepts a business selector. The same (`type`, `name`) twice in one direction, or a duplicated `service_type` within one struct, is a registry conflict that fails SDK initialization. An unregistered (`type`, `name`) reports `UNKNOWN_CONVERTER` and never falls back to `common`. Platform `service_type` members and placeholder values are mock stand-ins to verify against the real headers inside the internal network.
6. Copy input data when the lifetime requires it, store request-scoped values in `AlgContext`, and pack output into leased pool slots only through the documented ownership contract.

The Process batch bound is the framework constant `kMaxProcessBatchSize` (64); converters do not declare one.
[Operator creation](../../../../src/adapter/operator/operator_adapter.cpp) further caps the effective
Process batch limit at the output pool depth.

Converter parameters are an ordinary `Parameters<Params>` declaration (`params` on the Definition), read through `options.Params<Params>()`. Every string field of an output struct needs a `MaxBytes("field", ...)` size parameter named `<field>_max_bytes`, with a default between 1 and the platform maximum; the registry audit checks this.

For one required host slot, one business payload stream and one payload/result per request,
use `DecodeRequestRows` / `EncodeResultRows`
from `converter_authoring.h`. Business callbacks handle one owned payload or one borrowed output row;
helpers own looping, provenance, request IDs and diagnostic location. `OutputStringWriter`
uses actual pool capacities and explicit string lengths. Do not retain its borrowed view or pointers.
Independent metadata streams, multi-slot, expanded and aggregated conversions keep their explicit
algorithms and existing lower-level helpers; a single external slot alone does not imply a single stream.

Use `tests/contract/abi/test_cpp_operator_sdk.cpp`, `tests/contract/abi/test_operator_safety.cpp`, `tests/contract/abi/test_adapter_contract_security.cpp`, and existing modality converters as live templates. If the change also adds nodes, read `capability-nodes.md`; if it changes Core contract behavior, read `orchestration.md`.
