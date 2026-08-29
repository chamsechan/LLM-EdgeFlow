# Verification and Delivery

Apply this checklist after implementing a framework extension. `CONTRIBUTING.md` owns the shared
branch, RFC threshold, documentation, quality-gate, and delivery policy; this reference adds only
framework-specific evidence.

1. If the change meets the RFC threshold, keep its scope, layer mapping, interfaces/data flow, invariants, and tests current.
2. Confirm new production Nodes, Models, or Backends appear in `./build/alg_pipeline_tool catalog` through registration and Definition data, without Web or skill catalog edits.
3. Validate every affected Pipeline and inspect its plan:

   ```bash
   ./build/alg_pipeline_tool validate <pipeline.json>
   ./build/alg_pipeline_tool plan <pipeline.json>
   ```

4. Run the affected Smoke Profile through the real Demo and verify structured results when the change affects an executable business path.
5. Run focused tests during development, then the canonical delivery gate once:

   ```bash
   ./scripts/run_all_tests.sh
   ```

6. Record any non-default sanitizer, real-model, hardware, performance, or compatibility evidence required by the RFC. Do not substitute it for the canonical gate.
7. Finish RFC status and durable documentation according to `CONTRIBUTING.md`. Remote delivery remains out of scope unless explicitly requested; if requested, use `github-branch-merge`.
