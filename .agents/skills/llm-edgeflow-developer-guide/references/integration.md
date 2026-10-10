# Integration

Use this reference for public Operator structures, Converter behavior, I/O selection and parameters.
Read the affected sections of [business onboarding](../../../../doc/dev_guide/business_onboarding.md)
and [output allocation](../../../../doc/dev_guide/operator_output_allocation.md).

The complete Operator request/response is the SDK contract. Input converters validate and select
fields; output converters assemble and serialize responses. Demo holds carriers, invokes the SDK,
and displays or copies the result. Reusing a host struct does not imply compatible payload semantics.

1. Settle external fields, ownership, cardinality, limits and failure behavior under CONTRIBUTING.
2. The public API is `edgeflow/operator/interface.h`. Platform substitutes live only in
   `include/platform_mock/`; they require actual-header verification in the authorized internal network.
3. Keep the exported table functions `noexcept`, with both `catch (const std::exception&)` and
   `catch (...)` barriers in `operator_adapter.cpp`.
4. Register ordinary input callbacks under `src/adapter/input/` and output callbacks under
   `src/adapter/output/`, using `REGISTER_INPUT_CONVERTER` / `REGISTER_OUTPUT_CONVERTER`.
   Each direction identifies a registration by `(type, name)`. Its single slot has matching
   `type_suffix` and neutral `value_type`; logical ports are typed internal Blackboard keys.
   `ExternalInputSlot<Value>` / `ExternalOutputSlot<Value>` use owned values from
   `adapter/io_values.h`. Only binding files know platform types and their layouts.
5. Pipeline root `io.input` / `io.output` are nonempty arrays selecting `{type, name, params?}`; outputs also declare `inputs`.
   Selected converter ports form the mandatory `PipelineIoBoundary` passed to Core validation.
   Each input must publish distinct ports. At least one selected input struct declares request IDs;
   all input items pair by batch row, with matching IDs where present. Named services on structs with
   a declared `service_type` require an explicit name-to-enum entry in binding `services`.
   Converter Definitions do not declare platform enums. `common` or a struct without that
   member has no expected service value. Lookup never falls back to `common`.
6. Parameters use `Parameters<P>` and immutable `ParameterValues`. Parse, Prepare and Validate run
   during creation; `options.Params<P>()` supplies the same typed values to all Process calls.
   `Effective()` reads declared members after Prepare, and omits unset optional members.

Integration resolves model `file` and `.File()` parameters relative to the Pipeline JSON
directory before Core validation. Reject empty names, absolute/drive/UNC paths, every `..`
component and symlink escape; allow missing files. Without a directory, check only the relative
spelling. Resource existence is checked by the Model or Backend when it opens the file.

A unique carrier type accepts any nonempty host-key namespace. Repeated types within one direction
require `name.type` keys, so empty output pointers can be routed uniquely. Inputs additionally
validate service values before decoding. The batch bound is `min(max_frame_depth, 64)`.

For one payload/result per request, use `DecodeRequestRows` / `EncodeResultRows` from
`converter_authoring.h`. Multiple logical streams and aggregations retain explicit algorithms.
`SetInputValue<Host, Value>` copies validated carrier content into owned values; the input row
callback receives `const Value&` and publishes business payloads. Output callbacks fill `Value*`,
including ordinary `std::string` fields; `EncodeResultRows` or explicit `WriteOutputValue` calls
the writer registered with `SetOutputValue<Host, Value>` using the leased pool specification.
Bindings alone read/write platform members, string lengths, metadata, request IDs and layouts.
Operator supplies the request ID table as read-only options; Converters do not publish IDs.
Never retain host pointers across Process calls or store request-local pointers in pooled outputs.

Every output string has an integer `<field>_max_bytes` parameter with minimum 1 and a valid default.
Platform layouts declare only the hard maximum; Catalog/schema supplement the corresponding bound.
Allocator name, allocator parameters and metadata count/type are fixed in the converter slot.
`IoConverterRegistry::Audit()` checks all registrations and normalizes allocator parameters once,
including unselected registrations. Create consumes those immutable results. Optional outputs
always own pools and may omit host keys per row; their views retain batch row positions. All outputs
are published together after successful encoding; failure returns all leases.

The current mock platform has declarations in `include/platform_mock/`, traits and layout helpers
in `adapter/platform_value_binding.h`, and bindings in `operator_builtin_value_types.cpp`.
A different platform supplies its own binding files, reusing neutral Converter values when content
semantics match. Register matching `value_type` through `SetInputValue` / `SetOutputValue`.
Declare actual request ID/service members with
`SetRequestIdMember` / `SetServiceTypeMember`; do not infer memory layouts. New nested layouts use
`REGISTER_OPERATOR_OUTPUT_ALLOCATOR` and their own allocation/reset/budget implementation.
See the output allocation guide for ownership and actual-platform acceptance limits.

Use the existing Adapter/Operator/ABI tests, including converter contract tests and golden Operator
results. Cross-layer changes also read `orchestration.md`; new Nodes read `capability-nodes.md`.

Converter ports are local logical names. Output entries connect every required port to
`node.port` or `input.port`; callbacks read/write via `options.Port(logical_name)`. The immutable
selection owns these mappings. Unreferenced input ports return an empty name and skip publication.
