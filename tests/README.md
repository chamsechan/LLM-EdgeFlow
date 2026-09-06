# Test layout

Test paths describe ownership; CTest labels describe when and where a test runs.

- `unit/` contains focused suites grouped by the owning runtime component.
- `integration/` covers compositions that cross component or process boundaries.
- `contract/` protects public ABI, executable Catalog and architecture governance.
- `tooling/` covers developer-facing CLI or Studio behavior.
- `e2e/` contains opt-in physical model and hardware scenarios.
- `support/` contains test-only helpers that do not register production capabilities.
- `fixtures/` contains stable test data grouped by purpose rather than RFC stage.

Deterministic Model and Backend registrations shared by Demo mock profiles and tests live in
`dev_support/inference/`. They are OBJECT targets so every consumer receives the registration
translation units, while production `alg_sdk` never links them.

The source path for each compiled test is declared once in `cmake/TestInventory.cmake`.
`cmake/Tests.cmake` groups those sources into the default PCH-enabled runners;
`cmake/IndividualTests.cmake` creates process-per-file targets for focused diagnostics. Both modes
must satisfy the same required CTest inventory.

Add coverage to the narrowest existing suite that owns the behavior. Create a new executable only
when process isolation or an independent runtime lifecycle is part of the contract.

Operator allocation-failure tests use `support/scoped_allocation_failure.*`, linked only
into the adapter runner and the individual output-pool/value-registry test executables.
It replaces C++ allocation functions in those executables; the SDK, tools and demos use
the normal allocator without test hooks. Injection is one-shot, thread-local and scoped,
with automatic restoration for nested scopes and exception unwinding.

Arm injection only around a synchronous operation, outside GoogleTest assertions. Sweep
allocation positions until the operation succeeds without triggering injection; do not
encode container names or fixed allocation counts. Check rollback, unchanged observable
state and successful retry. For leak checks, destroy operation-owned objects while the
scope is still alive, then require both `!Overflowed()` and `Outstanding() == 0`.
The bounded pointer ledger observes C++ allocations on the current thread, not `malloc`
or resources owned by other threads; sanitizer checks remain complementary.
