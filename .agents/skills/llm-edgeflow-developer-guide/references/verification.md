# Verification and Delivery

Apply this checklist after implementing any framework extension.

1. Confirm the feature has an RFC under `doc/rfcs/`, with scope, layer mapping, interfaces/data flow, invariants, and tests kept current. Do not mark it Completed yet.
2. Confirm new production nodes/engines appear in `./build/alg_pipeline_tool catalog` through registration and Definition data, without Web or skill edits.
3. Validate every affected Pipeline and inspect its plan:

   ```bash
   ./build/alg_pipeline_tool validate <pipeline.json>
   ./build/alg_pipeline_tool plan <pipeline.json>
   ```

4. Run the affected Smoke Profile through the real Demo and verify structured results.
5. Format and run all tests:

   ```bash
   ./scripts/format.sh
   cmake -S . -B build
   cmake --build build -j4
   ctest --test-dir build --output-on-failure
   ./scripts/run_all_tests.sh
   ```

6. Update README capability documentation and Changelog for major features. Mark the RFC and RFC index Completed only after the full gate succeeds.
7. Do not upload or merge unless the user explicitly requests it. If requested, then use `github-branch-merge` and its repository script.
