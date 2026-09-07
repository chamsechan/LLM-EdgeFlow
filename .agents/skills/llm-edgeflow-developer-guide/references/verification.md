# Verification and Delivery

Apply this checklist after implementing a framework extension. `CONTRIBUTING.md` owns the shared
branch, RFC threshold, documentation, quality-gate, and delivery policy; this reference adds only
framework-specific evidence.

1. If the change meets the RFC threshold, keep its scope, layer mapping, interfaces/data flow, invariants, and tests current.
2. Confirm new production Nodes, Models, or Backends appear in `./build/alg_pipeline_tool catalog` through registration and Definition data, without Web or skill catalog edits.
3. Validate every affected Pipeline and inspect its plan with the target build's tool:

   ```bash
   ./build/alg_pipeline_tool validate <pipeline.json>
   ./build/alg_pipeline_tool plan <pipeline.json>
   ```

   For fixtures intentionally using test-only registrations, use `alg_pipeline_tool_test`;
   this is not a workaround for a failing production configuration. See
   [tool selection](../../../../tools/pipeline_studio/README.md#校验工具选择).
4. When a Demo-supported business path changes, run that edited Pipeline through the compatible
   Demo and check request IDs, status and expected output fields. For a C ABI-only path, use the
   corresponding end-to-end contract tests instead of adding a new Demo solely for verification. Follow
   [running the current solution](../../../../tools/pipeline_studio/README.md#运行当前方案)
   for `.conf` / Profile selection and Demo Control behavior; running an unchanged Profile
   does not verify a new JSON file.
5. Run focused tests during development, then the canonical delivery gate once:

   ```bash
   ./scripts/run_all_tests.sh
   ```

6. Record any non-default sanitizer, real-model, hardware, performance, or compatibility evidence required by the RFC. Do not substitute it for the canonical gate.
7. Finish RFC status and durable documentation according to `CONTRIBUTING.md`. Remote delivery remains out of scope unless explicitly requested; if requested, use `github-branch-merge`.

Report evidence at its actual level: static configuration/plan validation; executed business
path and checked results; real-model effects and target-platform acceptance where required.
The canonical gate disables the dedicated real-model E2E suite, and backend-specific tests can
skip without assets. Report skips and unverified scope rather than treating a passing gate or
Mock output as production acceptance. Use [effects verification](../../../../doc/VERIFIABLE_SELECTION.md)
and [RFC-0029](../../../../doc/rfcs/0029-external-readiness-and-intranet-sdk-migration.md) for the
applicable business and target-environment evidence; ordinary development does not require all
production acceptance work.
