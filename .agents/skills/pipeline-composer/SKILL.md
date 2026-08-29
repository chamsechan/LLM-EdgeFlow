---
name: pipeline-composer
description: Compose or modify LLM-EdgeFlow algorithm workflows using existing registered nodes and Pipeline JSON. Use for configuration-only pipeline creation, validation, planning, normalization, and smoke execution; route missing operator capabilities to llm-edgeflow-developer-guide.
---

# Pipeline Composer

Use the runtime Catalog and Validator as the only source of node, port, parameter, engine, model-capability, and business-contract facts. Do not maintain or infer a parallel catalog in this skill.

## Workflow

1. Build `alg_pipeline_tool` if it is unavailable, then query the target biz contract and its filtered assets. Prefer the current `--biz` spelling; `--business` is compatibility-only:

   ```bash
   ./build/alg_pipeline_tool catalog --biz <biz_name>
   ```

2. Inspect each plausible node before using it:

   ```bash
   ./build/alg_pipeline_tool describe-node <node_type>
   ```

3. Prefer cloning a compatible Profile; otherwise create an empty draft. Modify only Pipeline JSON and reuse registered nodes.

   ```bash
   ./build/alg_pipeline_tool init --biz <biz_name> --profile <profile_name>
   ./build/alg_pipeline_tool init --biz <biz_name> --empty
   ```

4. Runtime validation requires explicit `id` and `depends_on`. For a legacy ordered document,
   normalize through C++ before structural DAG edits; do not rely on implicit sequential parsing:

   ```bash
   ./build/alg_pipeline_tool normalize --explicit-dag <pipeline.json>
   ```

5. Validate after every meaningful edit. Use diagnostic `code`, JSON `path`, `node_id`, `port`, `related_nodes`, and `suggestions` to repair the document; do not reproduce validation rules in scripts or prompts.

   ```bash
   ./build/alg_pipeline_tool validate <pipeline.json>
   ./build/alg_pipeline_tool plan <pipeline.json>
   ```

6. Run the matching Smoke Profile only after validation succeeds:

   ```bash
   ./build/alg_demo --profile <smoke_profile>
   ```

For human composition, use `./show --web` or `./show <pipeline.json> --web`. For AI and automation, use `alg_pipeline_tool` and consume its versioned JSON output.

## Boundaries

- Do not guess Blackboard Keys, types, node parameters, model IDs, engine capabilities, or Adapter ingress/egress.
- Do not hand-edit a Catalog, Web node list, or this skill when nodes change; registration and Definition data must make assets discoverable.
- Do not generate node implementation code during configuration composition.
- If no Catalog composition can satisfy the contract, report the exact missing input/output or capability, stop editing Pipeline JSON, and route the task to `llm-edgeflow-developer-guide` for the relevant layer.
- Configuration-only composition normally does not require an RFC. Follow
  [`CONTRIBUTING.md`](../../../CONTRIBUTING.md) for branch, verification, documentation, and
  delivery decisions; do not invoke remote delivery unless the user explicitly asks.
