# Layer 1: C ABI and Adapter

Use this reference for new modalities, public C structures, Adapter behavior, business registration, or allowed Pipeline names.

1. Start from `doc/rfcs/RFC_TEMPLATE.md`; map the external contract, ownership, cardinality, batch bounds, and failure behavior before implementation.
2. Keep `include/company_alg_interface.h` valid C11. Expose only C primitives, fixed-layout C structs, pointers with documented ownership, and C enums—never STL or third-party types.
3. Preserve all six exported functions and their exception barrier in `src/adapter/company_c_adapter.cpp`: `noexcept`, `try`, `catch (const std::exception&)`, and `catch (...)`.
4. Implement business conversion through `IBusinessAdapter` under `src/adapter/adapters/`, using the existing adapters as the source of current patterns. Register through `REGISTER_BUSINESS_ADAPTER`; do not add a central dispatch switch.
5. Declare Adapter ingress/egress Blackboard ports and allowed runtime Pipeline names in the business Definition. The name in Pipeline JSON must be one accepted by the Adapter; a display name or Demo business alias is not a substitute.
6. Copy input data when the ABI lifetime requires it, store request-scoped values in `AlgContext`, and pack output only through the documented ownership contract.

Use `tests/test_adapter_contract_security.cpp`, `tests/test_c_abi_safety.cpp`, `tests/test_c11_abi_compliance.c`, and existing modality adapters as live templates. If the change also adds nodes, read `layer3-node.md`; if it changes Core contract behavior, read `layer2-pipeline.md`.
