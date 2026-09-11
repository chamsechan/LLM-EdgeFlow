# Integration

Use this reference for new modalities, public C structures, Adapter behavior, business registration, or allowed Pipeline names.

Business input/output means the complete public C ABI request/response, including serialized
payload semantics. `Unpack` and Adapter packing must implement that contract inside the SDK;
Demo/Python cannot perform the missing field selection or response assembly. Reusing a C struct
does not imply payload compatibility. Follow [the boundary and carrier distinction](../../../../doc/dev_guide/business_onboarding.md#输入输出以-c-abi-为边界).

Start with [business onboarding](../../../../doc/dev_guide/business_onboarding.md) to select the requested
integration path. Reuse the Adapter when the external contract is unchanged; an existing C ABI
path edit does not automatically require a new Operator or Demo path. Adding a production
Adapter to the current shared SDK does require a matching bridge: Operator `GlobalInit` audits
all registered Adapters. For new Operator host types, also register ValueType capacity,
initialization and release. An ABI-only registration mode would require separate design.
Use the existing `ResultPackingAdapter` and `MakeSingleSlotBizBridge` helpers where applicable;
the onboarding guide owns those implementation examples and optional Demo conversion steps.

1. Public C ABI, Operator contract, or new modality changes meet the RFC threshold in
   `CONTRIBUTING.md`. Map the external contract, ownership, cardinality, batch bounds, and
   failure behavior before implementation.
2. Keep `include/edgeflow/c_api.h` valid C11. Its current platform data declarations come from `include/platform_mock/`; see that directory's README for the distinction from real company headers. Keep existing local mock DTOs and platform enums there, and keep framework entrypoints under `edgeflow/`. Expose only C primitives, fixed-layout C structs, pointers with documented ownership, and C enums through C headers—never STL or third-party types.
3. Preserve all six exported functions and their exception barrier in `src/adapter/c_api_adapter.cpp`: `noexcept`, `try`, `catch (const std::exception&)`, and `catch (...)`.
4. Implement biz conversion through `IBizAdapter` under `src/adapter/biz/`, using existing adapters as current patterns. Register through `REGISTER_BIZ_ADAPTER`; do not add a central dispatch switch.
5. Declare Adapter ingress/egress Blackboard ports and allowed runtime Pipeline names through its `BizDefinition` entries. The `biz_name` in Pipeline JSON must be accepted by the Adapter; a display name or Demo alias is not a substitute.
6. Copy input data when the ABI lifetime requires it, store request-scoped values in `AlgContext`, and pack output only through the documented ownership contract.

Use `tests/contract/abi/test_adapter_contract_security.cpp`, `tests/contract/abi/test_c_abi_safety.cpp`, `tests/contract/abi/test_c11_abi_compliance.c`, and existing modality adapters as live templates. If the change also adds nodes, read `capability-nodes.md`; if it changes Core contract behavior, read `orchestration.md`.

RFC-0044 lifecycle: `Unpack` validates biz fields before converting to owned DTOs. The unused
internal `IBizAdapter::ValidateInput` hook was removed; migrate custom overrides into `Unpack`
and rebuild extensions. Reuse `adapter/biz_input_constraints.h` for channel/audio semantic limits.
Egress metadata describes the Adapter's internal consumption, including ranked aggregation;
retain runtime provenance and output-capacity checks after static validation.
