# Test layout

Test paths describe ownership; CTest labels describe when and where a test runs.

- `unit/` contains focused suites grouped by the owning runtime component.
- `integration/` covers compositions that cross component or process boundaries.
- `contract/` protects public ABI, executable Catalog and architecture governance.
  `contract/authoring/` checks compile-time Node authoring contracts and signature documentation.
- `tooling/` covers developer-facing CLI or Studio behavior.
- `e2e/` contains opt-in physical model and hardware scenarios.
- `support/` contains test-only helpers that do not register production capabilities.
- `fixtures/` contains stable test data grouped by purpose.

The four adapter parsing examples live in `support/adapter_examples/` and are compiled by
`AdapterContractSecurityTest`. Their example DTOs and keys do not register production businesses.

Deterministic Model and Backend registrations shared by Demo mock profiles and tests live in
`dev_support/inference/`. They are OBJECT targets so every consumer receives the registration
translation units, while production `alg_sdk` never links them.

`tests/RuntimeTests.cmake` collects `test_*.cpp` from each shared runner's owned directories
using `file(GLOB ... CONFIGURE_DEPENDS)`. Adding or removing a file in those directories
automatically updates the next build, including on CMake 3.19. Directory matching is not
recursive; process-isolated contracts, opt-in E2E tests and Pipeline tests with different
runner dependencies keep explicit source entries. Test-only authoring fixtures are generated
by `tests/ScaffoldFixtures.cmake` and also remain explicit.

CTest entries, filters and labels stay explicit in `tests/RuntimeTests.cmake`. A new GoogleTest
suite needs a matching CTest filter; compiling a file alone does not schedule its cases.
`TestLabelsContractTest` rejects compiled cases that no registered filter runs, and the
required CTest inventory is also checked. Precompiled headers are optional through
`LLM_EDGEFLOW_TEST_PCH`; they default to `OFF` and remain disabled in the canonical gate.

Add coverage to the narrowest existing suite that owns the behavior. Create a new executable only
when process isolation or an independent runtime lifecycle is part of the contract.

Keep a complete rule matrix in its owning component. CLI and HTTP wrappers use representative
cases to check status and diagnostic forwarding. `InitNodeForTest` prepares a validated plan
before calling `Init`, so its rejection of invalid configuration is preparation coverage.

Node behavior lives in its dedicated `unit/nodes/test_*_node.cpp` suite; authoring examples and
generated scaffold integration live in `unit/nodes/test_node_authoring_examples.cpp`.
`tooling/test_pipeline_cli.py` owns native command contracts and viewer output;
`tooling/test_pipeline_studio.py` owns HTTP, browser and filesystem transactions.
`PipelineContractsTest` schedules native Catalog, Validator and Authoring contracts.

## Fast feedback

Solution authors use the per-runner commands in
[the custom Node guide](../src/custom_nodes/README.md#本地快速验证) and
[the business onboarding guide](../doc/dev_guide/business_onboarding.md#7-最小验证).
Framework maintainers changing Init/Process diagnostics or planning build
`edgeflow_test_core_runner` and filter on `NodeBaseContractsTest.*` / `PipelineConfigTest.*`.
Source paths and runner membership are together in `tests/RuntimeTests.cmake`. The final gate
covers the complete default configuration even when development used a minimal build; follow
[CONTRIBUTING](../CONTRIBUTING.md#6-run-one-canonical-delivery-gate).

Allocation-failure tests use `support/scoped_allocation_failure.*`, linked into the Core/Model,
Node and Adapter runners. It replaces C++ allocation functions in those executables; the SDK,
tools and demos use the normal allocator without test hooks. Injection is one-shot, thread-local and scoped,
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
