# Test layout

Test paths describe ownership; CTest labels describe when and where a test runs.

- `unit/` contains focused suites grouped by the owning runtime component.
- `integration/` covers compositions that cross component or process boundaries.
- `contract/` protects public ABI, executable Catalog and architecture governance.
- `tooling/` covers developer-facing CLI or Studio behavior.
- `e2e/` contains opt-in physical model and hardware scenarios.
- `support/` contains test-only helpers that do not register production capabilities.
- `fixtures/` contains stable test data grouped by purpose rather than RFC stage.

The four adapter parsing examples live in `support/adapter_examples/` and are compiled by
`AdapterContractSecurityTest`. Their example DTOs and keys do not register production businesses.

Deterministic Model and Backend registrations shared by Demo mock profiles and tests live in
`dev_support/inference/`. They are OBJECT targets so every consumer receives the registration
translation units, while production `alg_sdk` never links them.

The source path for each compiled test is declared once in `cmake_ext/TestInventory.cmake`.
`cmake_ext/Tests.cmake` groups those sources into the default PCH-enabled runners;
`cmake_ext/IndividualTests.cmake` creates process-per-file targets for focused diagnostics. Both modes
must satisfy the same required CTest inventory.

Add coverage to the narrowest existing suite that owns the behavior. Create a new executable only
when process isolation or an independent runtime lifecycle is part of the contract.

## Fast feedback for solution authors

Use the default sharded runners below while developing; replace the filter with the suite/test
you actually changed. Source inventory is in `cmake_ext/TestInventory.cmake`, runner membership in
`cmake_ext/Tests.cmake`.

| Change | Build target | Typical GoogleTest filter |
| --- | --- | --- |
| Node algorithm, fields or Control handler | `edgeflow_test_nodes_runner` | `CommonNodesTest.*` or the affected Node suite |
| Init/Process diagnostic or planning | `edgeflow_test_core_runner` | `NodeBaseContractsTest.*` / `PipelineConfigTest.*` |
| Adapter, protocol copies or Operator bridge | `edgeflow_test_adapter_runner` | `OperatorBizBridgeRegistryTest.*` / `OperatorApiTest.*` |
| Demo result conversion or Pipeline integration | `edgeflow_test_tooling_runner` | `DemoRunnerTest.*` |

```bash
cmake --build build --target edgeflow_test_nodes_runner -j 4
./build/edgeflow_test_nodes_runner --gtest_list_tests
./build/edgeflow_test_nodes_runner --gtest_filter='CommonNodesTest.*'
```

The [Node helper](support/node_test_utils.h) initializes a registered Node with a validated Plan and Session. Put
request input into a fresh `AlgContext`, call Process, and assert actual outputs and
`(req_id, sub_id)`; do not stop at factory creation. Cover the algorithm's empty/invalid input and
failure behavior. The generator's `--write-test` creates a complete standalone test file; add
`--add-to-cmake` to register both source and test. Rebuild `alg_pipeline_tool` after a production registration/Definition change, and check
the composed solution with the same build. The final gate covers the complete default configuration
even when first practice used a minimal build. Follow [CONTRIBUTING](../CONTRIBUTING.md#6-run-one-canonical-delivery-gate)
to run it directly for a local handoff or through the authorized PR delivery script.

Operator allocation-failure tests use `support/scoped_allocation_failure.*`, linked only
into the adapter runner and the individual output-pool/value-registry test executables.
It replaces C++ allocation functions in those executables; the SDK, tools and demos use
the normal allocator without test hooks. Injection is one-shot, thread-local and scoped,
with automatic restoration for nested scopes and exception unwinding.

For ordinary Operator end-to-end tests, derive from
[`OperatorTestFixture`](support/operator_test_fixture.h) and create a `ScopedTestOperator(ops_)`.
`Create(conf)` uses root `.` / CPU / depth 25 / device 0, with explicit overrides available.
Assert the returned status and `create_diagnostic()`; use `get()` for the real Operator calls.
Keep business DTOs, named slots and complete response assertions in the test. The
[Golden tests](integration/operator/test_operator_golden.cpp) and
[DocQA test](integration/pipeline/test_doc_qa_rerank.cpp) are compiled examples.

Declare the scoped handle before outputs so assertion failures release output leases first.
Wait for outstanding calls and release every output reference before asserting `Close()`;
its diagnostic is available via `close_diagnostic()`. The destructor performs fallback cleanup
and reports failure. Only the outer fixture owns global `Init/DeInit`; closing one handle must
not deinitialize another. Capacity, concurrency and lifecycle contract tests keep explicit API
control. This helper neither copies outputs nor changes SDK depth or error semantics.

Arm injection only around a synchronous operation, outside GoogleTest assertions. Sweep
allocation positions until the operation succeeds without triggering injection; do not
encode container names or fixed allocation counts. Check rollback, unchanged observable
state and successful retry. For leak checks, destroy operation-owned objects while the
scope is still alive, then require both `!Overflowed()` and `Outstanding() == 0`.
The bounded pointer ledger observes C++ allocations on the current thread, not `malloc`
or resources owned by other threads; sanitizer checks remain complementary.

On ELF platforms the existing Model/Backend runner wraps `posix_memalign` at link time to
exercise ENOMEM without requesting huge allocations. The thread-local one-shot failure switch
is defined only in `test_model_backend_decoupling.cpp`; production binaries keep their allocator.
`QualityGateScriptsContractTest` checks canonical and sanitizer script behavior, cached test
settings, empty-test rejection, failure propagation and the CI evidence fields.
