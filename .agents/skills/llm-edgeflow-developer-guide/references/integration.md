# Integration

Use this reference for Operator, Converter, I/O parameters, host layouts and output ownership.
Read only the affected current contracts:

| Work | Owning guide |
| --- | --- |
| Complete request/response semantics and converter authoring | [Business onboarding](../../../../doc/dev_guide/business_onboarding.md) |
| Host keys, layouts, allocation, capacity and lifecycle | [Output allocation](../../../../doc/dev_guide/operator_output_allocation.md) |
| Protocol shell, neutral Runtime and publication | [Integration architecture](../../../../doc/architecture.md#统一-operator-门面与运行时架构) |
| Public headers and selected shell/fixtures | [Source layout](../../../../doc/dev_guide/source_layout.md#接入适配层) |
| Deployment and model file resolution | [Configuration paths](../../../../configs/README.md#配置路径) |
| Framework failures and platform mapping | [Error handling](../../../../doc/dev_guide/error_handling.md) |

Implementation checklist:

1. Define the complete external payload, cardinality, limits, failure behavior and ownership.
   Reusing a host DTO does not establish compatible payload semantics. Demo must not select
   business fields or assemble the SDK response.
2. Keep platform types and layout access in the protocol shell/binding. Runtime receives neutral
   options, inputs and outputs. Function table pointer types omit `noexcept`; entry implementations
   retain `noexcept` and both standard and unknown exception barriers. Verify actual platform
   declarations only in the authorized internal network under the root repository guardrails.
3. Register each input/output converter with its typed ports, one host slot and immutable parameters.
   The selected converter ports form Core's explicit boundary; avoid central business dispatch.
4. Use the [row helpers](../../../../doc/dev_guide/business_onboarding.md#3-实现并注册转换器)
   after the business-specific validation and alignment. Preserve original optional row positions,
   diagnostics and whole-batch publication; do not retain host or request-local pointers.
5. For a new layout, implement the binding's read/write, allocation, reset and budget callbacks
   against actual capacity. Keep output leases and handle lifetime separate.
6. Verify complete Operator responses, row association, invalid input and failure cleanup in the
   existing Adapter/Operator suites. Layout or public entry changes also require ABI, ownership,
   publication and shell-consistency evidence appropriate to the change.

Cross-layer work also reads [Orchestration](orchestration.md); new algorithms use the
[Node skill](../../edgeflow-node-developer/SKILL.md). Verification follows
[the shared workflow](verification.md), using the user's actual edited configuration.
