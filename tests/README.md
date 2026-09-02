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
