---
name: pipeline-composer
description: Compose and run LLM-EdgeFlow solutions using registered nodes and biz contracts. Covers Pipeline JSON, necessary deployment conf files, optional Demo Profiles, validation and smoke execution; route capability gaps or new I/O conversion code to llm-edgeflow-developer-guide.
---

# Pipeline Composer

Use the runtime Catalog and Validator as the source of node, port, Pipeline parameter,
Model/Backend and business-contract facts. Do not maintain a parallel catalog in this skill.
For deployment `.conf` semantics, follow the [existing integration guide](../../../doc/developer_guide.md)
and native Resolver; do not reproduce their validation rules.

## Workflow

1. Build the tool if unavailable, and rebuild after registration changes. Query the target biz
   contract and its filtered assets:

   ```bash
   ./build/alg_pipeline_tool catalog --biz <biz_name>
   ```

   Use the production tool for the target build. For fixtures deliberately using test-only
   Models/Backends, use `alg_pipeline_tool_test` throughout discovery, init, validate and plan.
   Do not switch to test registrations to bypass a production configuration failure; inspect
   the diagnostics and target build's Catalog. [Tool selection and commands](../../../tools/pipeline_studio/README.md#校验工具选择).

2. Inspect each plausible node before using it:

   ```bash
   ./build/alg_pipeline_tool describe-node <node_type>
   ```

3. Prefer cloning a compatible Profile; otherwise create an empty draft. Reuse registered nodes.
   Limit edits to the requested Pipeline and necessary `.conf` / optional Profile configuration.
   Cloning a Pipeline does not retarget the source Profile.

   ```bash
   ./build/alg_pipeline_tool init --biz <biz_name> --profile <profile_name>
   ./build/alg_pipeline_tool init --biz <biz_name> --empty
   ```

   `init` normally returns a versioned response containing `pipeline`. To save a
   runtime document directly, use `--raw` and a new destination (do not overwrite
   an existing solution):

   ```bash
   ./build/alg_pipeline_tool init --biz <biz_name> --profile <profile_name> --raw > <new_pipeline.json>
   ```

   Check the command's exit status before using the file, then validate the saved
   document. An empty draft needs nodes and bindings before it can validate.

4. Every node must declare a non-empty `id` and an explicit `depends_on` array. Validate after every meaningful edit. Use diagnostic `code`, JSON `path`, `node_id`, `port`, `related_nodes`, and `suggestions` to repair the document; do not reproduce validation rules in scripts or prompts.

   ```bash
   ./build/alg_pipeline_tool validate <pipeline.json>
   ./build/alg_pipeline_tool plan <pipeline.json>
   ```

5. After validation, run the edited Pipeline through a compatible Demo. Follow
   [running the current solution](../../../tools/pipeline_studio/README.md#运行当前方案): confirm
   `.conf` `data.pipe_path` resolves to the edited JSON, inspect inherited model path overrides
   and capacities, and select a matching biz and dataset. Use
   `alg_pipeline_tool resolve-conf <edited.conf> --root <deployment_root> --depth <max_batch_or_depth>`
   to inspect the native resolved paths, their sources and normalized defaults; it does not load
   weights. Studio can save a JSON + `.conf` pair and command via “另存为可运行方案”; its model
   directory is explicit (`models` normally, `.` for project-relative fixtures), and model paths
   come from the edited Pipeline. For example:

   ```bash
   ./build/alg_demo --profile <compatible_profile> --config <edited.conf> --no-default-control --output-dir <run_output_dir>
   ```

   A new Profile is optional; explicit `--biz`, `--config` and `--dataset` also work. Use the
   original Profile alone only when its configuration already points to the intended Pipeline.
   Demo uses the selected Pipeline defaults; `--no-default-control` remains a compatibility
   option. Use `--example-control` only for the built-in update demonstration, and provide a
   Control file only when it is part of the requested scenario. Verify
   request IDs, status and expected output fields in `results.jsonl` and `summary.json`.

For human composition, use `./show --web` or `./show <pipeline.json> --web`. For AI and automation, use `alg_pipeline_tool` and consume its versioned JSON output.

## Boundaries

- Do not guess Blackboard Keys, types, node parameters, model IDs, engine capabilities, or Adapter ingress/egress.
- Do not hand-edit a Catalog, Web node list, or this skill when nodes change; registration and Definition data must make assets discoverable.
- Do not generate node implementation code during configuration composition.
- If no Catalog composition can satisfy the contract, report the exact missing input/output or capability, stop editing Pipeline JSON, and route the task to `llm-edgeflow-developer-guide` for the relevant layer.
- Report configuration validation, actual execution/results and real-model or target-platform
  acceptance separately. Smoke success does not prove business quality; use the
  [selection and effects workflow](../../../doc/VERIFIABLE_SELECTION.md) when effects acceptance
  is requested. Missing runtime assets leave execution unverified, even if static validation passes.
- Configuration-only composition normally does not require an RFC. Follow
  [`CONTRIBUTING.md`](../../../CONTRIBUTING.md) for branch, verification, documentation, and
  delivery decisions; do not invoke remote delivery unless the user explicitly asks.
