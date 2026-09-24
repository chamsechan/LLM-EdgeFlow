# Pipeline composition command reference

Read only the sections needed for the current change. Commands run from the repository root.
For deployment semantics, use [the integration guide](../../../../doc/developer_guide.md)
and native Resolver rather than reproducing their validation rules here.

First compare the requested complete Operator SDK input/output contract with the existing Adapter / Converters.
The same carrier layout is not enough: payload fields, types and serialization must also match.
Catalog ingress/egress are internal ports. A missing external conversion belongs in Integration;
do not compensate by extracting request fields or assembling business responses in Demo/Python.
See [the I/O boundary](../../../../doc/dev_guide/business_onboarding.md#输入输出以-operator-接口为边界).

## Discover assets

Build the tool if unavailable/stale, and rebuild after registration changes. Query the target
biz contract and its filtered assets:

```bash
./build/alg_pipeline_tool catalog --io-binding <binding_id>
```

Use the production tool for the target build. For fixtures deliberately using test-only
Models/Backends, use `alg_pipeline_tool_test` throughout discovery, init, validate and plan.
Do not switch to test registrations to bypass a production configuration failure; inspect
the diagnostics and target build's Catalog. [Tool selection and commands](../../../../tools/pipeline_studio/README.md#校验工具选择).

Inspect the candidate nodes needed for this change; reuse descriptions already read from the
same unchanged target build:

```bash
./build/alg_pipeline_tool describe-node <node_type>
```

## Create or clone a solution

For a new solution, prefer cloning a compatible Profile; otherwise create an empty draft.
For an existing solution, edit the requested files instead of initializing another one.
Reuse registered nodes; cloning a Pipeline does not retarget the source Profile.

```bash
./build/alg_pipeline_tool init --io-binding <binding_id> --profile <profile_name>
./build/alg_pipeline_tool init --io-binding <binding_id> --empty
```

`init` normally returns a versioned response containing `pipeline`. To save a
runtime document directly, use `--raw` and a new destination (do not overwrite
an existing solution):

```bash
./build/alg_pipeline_tool init --io-binding <binding_id> --profile <profile_name> --raw > <new_pipeline.json>
```

Check the command's exit status before using the file, then validate the saved
document. An empty draft needs nodes and bindings before it can validate.

## Validate changed inputs

Every node declares a non-empty `id` and explicitly maps required `inputs`; `outputs` names
its produced data. The Validator derives data dependencies; optional `depends_on` adds only
extra ordering constraints. Model references are explicit, while capability comes from the
registered model type. `max_parallel_workers` defaults to 1. Validate after a
coherent change and before execution; an unchanged already-validated document with unchanged
registrations need not be revalidated between unrelated commands. Use diagnostic `code`, JSON
`path`, `node_id`, `port`, `related_nodes`, and `suggestions` to repair the document; do not
reproduce Validator rules in scripts or prompts. The final delivery gate remains required.

```bash
./build/alg_pipeline_tool validate <pipeline.json>
./build/alg_pipeline_tool plan <pipeline.json>
```

## Run the intended configuration

After validation, run the edited Pipeline through a compatible Demo. Follow
[running the current solution](../../../../tools/pipeline_studio/README.md#运行当前方案): confirm
`.conf` `pipe_path` resolves to the edited JSON, inspect pipeline-owned `deployment` (io_binding,
out_mem, and model_paths), and select a matching dataset; Demo derives its runner from the configuration. Use
`alg_pipeline_tool resolve-conf <edited.conf> --root <deployment_root> --depth <max_batch_or_depth>`
to inspect the native resolved paths, their sources and normalized defaults; it does not load
weights. Studio can save a JSON + `.conf` pair and command via “另存为可运行方案”; its model
directory is explicit (`models` normally, `.` for project-relative fixtures), and model paths
come from the edited Pipeline. For example:

```bash
./build/alg_demo --profile <compatible_profile> --config <edited.conf> --output-dir <run_output_dir>
```

A new Profile is optional; explicit `--config` and `--dataset` also work. Use the
original Profile alone only when its configuration already points to the intended Pipeline.
Execution settings `chip`, `device_id`, `batch_size`, and `depth` come only from Profile JSON;
there are no corresponding CLI options. Without a Profile, Demo uses CPU, device 0, batch 1,
and depth 1. Use `--profiles-file <path> --profile <name>` to select different execution settings.
Demo uses the selected Pipeline defaults (by default, no example Control is sent). Use
`--example-control` only for the built-in update demonstration, and provide a Control file
only when it is part of the requested scenario. Verify request IDs, status and expected
output fields in `results.jsonl` and `summary.json`.

For human composition, use `./show --web` or `./show <pipeline.json> --web`. For AI and automation, use `alg_pipeline_tool` and consume its versioned JSON output.
