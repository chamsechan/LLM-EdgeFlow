#!/usr/bin/env python3
"""Task recipes for solution developers in LLM-EdgeFlow.

Provides task navigation, preparation, and verification for supported recipes:
- prompt-config: adjust prompts using existing nodes within identical biz contracts
- text-llm-node: create a custom LLM node (TextBatch -> TextBatch) with full scaffolding

Both recipes require a single-output mem_que deployment. Pipelines or confs with
data.outputs are rejected early with UNSUPPORTED_RECIPE_DEPLOYMENT.
"""

import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
from typing import Any, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[1]
SCAFFOLD_SCRIPT = ROOT / "scripts/scaffold_custom_node.py"
SPEC = importlib.util.spec_from_file_location("scaffold", SCAFFOLD_SCRIPT)
SCAFFOLD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SCAFFOLD)

VERIFY_SELECTION_SCRIPT = ROOT / "tools/verify_selection.py"
V_SPEC = importlib.util.spec_from_file_location("verify_selection", VERIFY_SELECTION_SCRIPT)
VERIFY_SELECTION = importlib.util.module_from_spec(V_SPEC)
V_SPEC.loader.exec_module(VERIFY_SELECTION)

SUPPORTED_RECIPES = {
    "prompt-config": {
        "title": "Prompt Configuration Recipe",
        "description": "Adjust prompt templates and configurations using existing nodes within identical biz contracts.",
        "preconditions": "Requires a verified data.mem_que Profile and valid Pipeline.",
        "artifacts": "Pipeline JSON, pipeline .conf, effects sample, verification command.",
    },
    "text-llm-node": {
        "title": "Text LLM Node Recipe",
        "description": "Create a custom LLM node handling TextBatch -> TextBatch with tests and pipeline deployment.",
        "preconditions": "Requires TextBatch 1:1 preserve ports, registered LLM model capability, and data.mem_que.",
        "artifacts": "Node source, unit test, CMake registration, Pipeline JSON, .conf, effects sample.",
    },
}

UNSUPPORTED_RECIPE_DEPLOYMENT = "UNSUPPORTED_RECIPE_DEPLOYMENT"
# These are labelled task fixtures, not an alternate capability catalog.
DEFAULT_EFFECTS = {
    "keyword_match_rules": "tests/fixtures/effects/keyword_exact.json",
    "entity_extract_mock": "tests/fixtures/effects/entity_mock_exact.json",
    "entity_extract_custom_mock": "tests/fixtures/effects/entity_mock_exact.json",
}


class RecipeError(ValueError):
    def __init__(self, message, report=None, code="RECIPE_FAILED"):
        super().__init__(message)
        self.report = report
        self.code = code


def read_json_file(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def absolute(path, root):
    path = Path(path)
    return (root / path).resolve() if not path.is_absolute() else path.resolve()


def check_unsupported_deployment(conf_path):
    conf = read_json_file(conf_path)
    data = conf.get("data", {})
    if "outputs" in data:
        return {"error_code": UNSUPPORTED_RECIPE_DEPLOYMENT,
                "message": "This recipe supports only single-output data.mem_que; use the native Operator workflow for data.outputs."}
    if not isinstance(data.get("mem_que"), dict):
        raise RecipeError("A data.mem_que deployment is required")
    return None


def require_deployment(conf_path):
    unsupported = check_unsupported_deployment(conf_path)
    if unsupported:
        raise RecipeError(unsupported["message"], code=unsupported["error_code"])
    return read_json_file(conf_path)["data"]["mem_que"]


def get_profile_data(profile_name, root=ROOT):
    profiles = read_json_file(root / "demo/profiles.json").get("profiles", {})
    if profile_name not in profiles:
        raise RecipeError(f"Unknown Profile: {profile_name}")
    profile = profiles[profile_name]
    conf = absolute(profile["config"], root)
    data = read_json_file(conf)["data"]
    pipeline = absolute(data.get("pipe_path", ""), root)
    return profile, conf, pipeline


def list_recipes(as_json=False):
    if as_json:
        print(json.dumps({"schema_version": 1, "ok": True, "recipes": SUPPORTED_RECIPES}))
    else:
        for name, recipe in SUPPORTED_RECIPES.items():
            print(f"{name}: {recipe['title']}\n  {recipe['description']}")
    return 0


def native(tool, arguments, root, document=None):
    process = subprocess.run([str(tool), *arguments], cwd=root,
                             input=json.dumps(document) if document is not None else None,
                             capture_output=True, text=True, check=False, timeout=60)
    try:
        report = json.loads(process.stdout)
    except (ValueError, TypeError) as error:
        raise RecipeError(f"Native tool returned no JSON: {process.stdout}\n{process.stderr}") from error
    if process.returncode or not report.get("ok", False):
        raise RecipeError("Native " + arguments[0] + " failed", report)
    return report


def tool_context(tool_path, build_dir, root, require_tool=True, demo_path=None):
    build = absolute(build_dir, root)
    tool = absolute(tool_path, root)
    if tool.name not in ("alg_pipeline_tool", "alg_pipeline_tool_test") or tool.parent != build:
        raise RecipeError("--tool must select alg_pipeline_tool or alg_pipeline_tool_test in --build-dir")
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        raise RecipeError(f"Configure --build-dir first: {cache} is missing")
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL=") and Path(line.split("=", 1)[1]).resolve() != root:
            raise RecipeError("--build-dir belongs to another source tree")
    if require_tool and not tool.is_file():
        raise RecipeError(f"Build the selected tool first: cmake --build {build} --target {tool.name}")
    demo = absolute(demo_path, root) if demo_path is not None else build / "alg_demo"
    if demo != build / "alg_demo":
        raise RecipeError("--demo must be alg_demo from the selected --build-dir")
    return tool, build, demo


def deployment_root(root, pipeline, model_root):
    return Path(os.path.commonpath([root, pipeline.parent, model_root])).resolve()


def make_recipe_conf(pipeline_doc, mem_que, pipeline_target, root, model_root=None):
    root = root.resolve()
    pipeline = absolute(pipeline_target, root)
    models = absolute(model_root, root) if model_root is not None else root / "models"
    bundle = deployment_root(root, pipeline, models)
    return VERIFY_SELECTION.build_run_conf(pipeline_doc, mem_que,
                                           pipeline.relative_to(bundle), models, bundle)


def command(description, argv):
    argv = [str(arg) for arg in argv]
    return {"description": description, "argv": argv,
            "command": " ".join(shlex.quote(arg) for arg in argv)}


def result(recipe, completed, pending, artifacts=(), error=None, step=None, **extra):
    report = {"schema_version": 1, "ok": error is None, "recipe": recipe,
              "completed_steps": completed, "pending_steps": pending,
              "failed_step": step if error is not None else None,
              "artifacts": [str(p) for p in artifacts], "next_commands": []}
    if error is not None:
        report.update(message=str(error), error_code=getattr(error, "code", "RECIPE_FAILED"))
        if getattr(error, "report", None) is not None:
            report["report"] = error.report
    report.update(extra)
    return report


def effects_inputs(profile_name, pipeline, root, effects_path, model_root, manifest_path):
    default = DEFAULT_EFFECTS.get(profile_name)
    if effects_path is None and default is None:
        raise RecipeError("Supply --effects with independently labelled business outputs for this Profile")
    source = absolute(effects_path or default, root)
    spec = read_json_file(source)
    if spec.get("biz_name") != pipeline["biz_name"]:
        raise RecipeError("Effects biz_name does not match the selected Profile")
    check_effects(spec)
    dataset = absolute(spec["dataset"], source.parent)
    if not dataset.is_file():
        raise RecipeError(f"Effects dataset is missing: {dataset}")
    is_fixture = profile_name in ("entity_extract_mock", "entity_extract_custom_mock")
    models = absolute(model_root, root) if model_root is not None else (root if is_fixture else root / "models")
    manifest = absolute(manifest_path, root) if manifest_path is not None else root / (
        "tests/fixtures/asset_manifest_test.json" if is_fixture else "models/asset_manifest.json")
    return spec, dataset, models, manifest


def check_effects(spec):
    samples = spec.get("samples", [])
    if spec.get("schema_version") != 1 or not samples:
        raise RecipeError("Effects requires schema_version=1 and nonempty labelled samples")
    ids = set()
    for sample in samples:
        request_id = sample.get("request_id")
        checks = sample.get("expected", {})
        if type(request_id) is not int or request_id in ids:
            raise RecipeError("Effects request IDs must be unique integers")
        ids.add(request_id)
        if not isinstance(checks, dict) or not any(key.startswith("/output/") for key in checks):
            raise RecipeError("Each sample needs an independent /output/... business expectation; /status alone is insufficient")
    minimum = spec.get("minimum_pass_rate", 1.0)
    if type(minimum) not in (int, float) or not 0 < minimum <= 1:
        raise RecipeError("minimum_pass_rate must be in (0, 1]")


def select_llm_node(pipeline, catalog):
    definitions = {node["node_type"]: node for node in catalog["nodes"]}
    choices = []
    for index, node in enumerate(pipeline["pipeline"]):
        definition = definitions.get(node["node_type"], {})
        if definition.get("model_capability") != "llm":
            continue
        inputs, outputs = definition.get("inputs", []), definition.get("outputs", [])
        if len(inputs) != 1 or len(outputs) != 1:
            continue
        if any(port.get("type_id") != "TextBatch" or port.get("cardinality") != "1:1" or
               port.get("provenance_policy") != "preserve" or port.get("lifetime") != "request"
               for port in [*inputs, *outputs]):
            continue
        field = definition.get("model_config_field")
        model_id = node.get("config", {}).get(field)
        if model_id is None:
            model_id = next((f.get("default") for f in definition.get("config_fields", []) if f["name"] == field), None)
        if not model_id:
            continue
        bindings = node.get("ports", {})
        in_key = bindings.get("inputs", {}).get(inputs[0]["key"], inputs[0]["key"])
        out_key = bindings.get("outputs", {}).get(outputs[0]["key"], outputs[0]["key"])
        choices.append((index, model_id, in_key, out_key))
    if len(choices) != 1:
        raise RecipeError("text-llm-node requires exactly one Catalog-registered TextBatch 1:1 preserve LLM replacement point")
    return choices[0]


def prepare(recipe, name, profile_name, tool_path, build_dir, pipeline_target, root,
            effects_path=None, model_root=None, manifest_path=None):
    root = root.resolve()
    step, completed = "preconditions", []
    try:
        if recipe == "text-llm-node":
            if not re.fullmatch(r"[A-Z][A-Za-z0-9]*", name):
                raise RecipeError("Node name must be a PascalCase C++ identifier")
            name = name if name.endswith("Node") else name + "Node"
        _, source_conf, _ = get_profile_data(profile_name, root)
        mem_que = require_deployment(source_conf)  # Reject outputs before tools or writes.
        tool, build, demo = tool_context(tool_path, build_dir, root)
        catalog = native(tool, ["catalog"], root)
        profile = next((p for p in catalog["profiles"] if p["name"] == profile_name), None)
        if profile is None or absolute(profile["config"], root) != source_conf:
            raise RecipeError("Profile is not available in the selected executable Catalog")
        pipeline = native(tool, ["init", "--biz", profile["pipeline_biz"], "--profile", profile_name], root)["pipeline"]
        native(tool, ["validate", "--stdin"], root, pipeline)
        deployment_preview = copy.deepcopy(pipeline)
        target = absolute(pipeline_target, root)
        if target.suffix != ".json":
            raise RecipeError("--pipeline must end in .json")
        conf_target = target.with_suffix(".conf")
        effects_target = target.with_name(target.stem + "_effects.json")
        spec, dataset, models, manifest = effects_inputs(profile_name, pipeline, root, effects_path, model_root, manifest_path)
        selection = VERIFY_SELECTION.inspect_selection(pipeline, tool, models, manifest)
        if not selection["ok"]:
            raise RecipeError("Selected build or model assets are not verified", selection)
        plan = SCAFFOLD.ChangePlan()
        generated = []
        if recipe == "text-llm-node":
            if any(node["node_type"] == name for node in catalog["nodes"]):
                raise RecipeError(f"Node type already registered: {name}")
            index, model_id, in_key, out_key = select_llm_node(pipeline, catalog)
            node = pipeline["pipeline"][index]
            node["node_type"] = name
            node["config"] = {"bind_model": model_id}
            node["ports"] = {"inputs": {"input": in_key}, "outputs": {"output": out_key}}
            snake = SCAFFOLD.to_snake_case(name)
            src = root / "src/custom_nodes" / (snake + ".cpp")
            test = root / "tests/unit/nodes" / ("test_" + snake + ".cpp")
            in_port, out_port = ("input", "TextBatch", "1:1", "preserve"), ("output", "TextBatch", "1:1", "preserve")
            description = f"{name} custom LLM node"
            plan.add_new_file(src, SCAFFOLD.render_model_node(name, description, "llm", in_port, out_port))
            plan.add_new_file(test, SCAFFOLD.render_standalone_test(name, description, "model", "llm", in_port, out_port))
            for path, update, filename in [
                (root / "src/custom_nodes/CMakeLists.txt", SCAFFOLD.updated_cmakelists, src.name),
                (root / "cmake_ext/CustomNodeTests.cmake", SCAFFOLD.updated_custom_node_tests_cmake, test.name),
            ]:
                plan.add_modification(path, path.read_text(encoding="utf-8"), update(path, filename))
            generated = [src, test]
        conf = make_recipe_conf(pipeline, mem_que, target, root, models)
        spec = copy.deepcopy(spec)
        spec["name"] = name + "_effects"
        spec["dataset"] = os.path.relpath(dataset, effects_target.parent)
        # Check deployment fields against the compiled source graph. A newly generated
        # Node becomes visible to native validation only after verify rebuilds the tool.
        bundle = deployment_root(root, target, models)
        with tempfile.TemporaryDirectory(prefix=".recipe-preview-", dir=root) as temporary:
            temp = Path(temporary)
            preview_pipeline = temp / "pipeline.json"
            preview_pipeline.write_text(json.dumps(deployment_preview), encoding="utf-8")
            preview_conf = VERIFY_SELECTION.build_run_conf(deployment_preview, mem_que, preview_pipeline.relative_to(bundle), models, bundle)
            (temp / "pipeline.conf").write_text(json.dumps(preview_conf), encoding="utf-8")
            native(tool, ["resolve-conf", str((temp / "pipeline.conf").relative_to(bundle)), "--root", str(bundle)], root)
        for path, document in [(target, pipeline), (conf_target, conf), (effects_target, spec)]:
            plan.add_new_file(path, json.dumps(document, ensure_ascii=False, indent=2) + "\n")
        completed.append("preconditions")
        step = "files_prepared"
        plan.commit()
        completed.append(step)
        argv = [sys.executable, root / "scripts/dev_recipe.py", "verify", recipe,
                "--pipeline", target, "--tool", tool, "--build-dir", build,
                "--effects", effects_target, "--model-root", models, "--manifest", manifest, "--demo", demo]
        if recipe == "text-llm-node":
            argv += ["--name", name]
        return result(recipe, completed, ["verify"], [*generated, target, conf_target, effects_target],
                      next_commands=[command("Build, validate, test and evaluate this prepared task", argv)])
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        return result(recipe, completed, [step, "verify"], error=error, step=step)


def prepare_prompt_config(name, profile_name, tool_path, build_dir, pipeline_target, root=ROOT,
                          effects_path=None, model_root=None, manifest_path=None):
    return prepare("prompt-config", name, profile_name, tool_path, build_dir, pipeline_target, root,
                   effects_path, model_root, manifest_path)


def prepare_text_llm_node(name, profile_name, tool_path, build_dir, pipeline_target, root=ROOT,
                         effects_path=None, model_root=None, manifest_path=None):
    return prepare("text-llm-node", name, profile_name, tool_path, build_dir, pipeline_target, root,
                   effects_path, model_root, manifest_path)


def verify_recipe(recipe, pipeline_path, tool_path, build_dir, effects_path, model_root,
                  name=None, demo_path=None, manifest_path=None, root=ROOT):
    root = root.resolve()
    steps = ["preconditions", "build", "catalog_discovery", "config_validation", "focused_test", "evaluate"] if recipe == "text-llm-node" else ["preconditions", "config_validation", "evaluate"]
    completed, step = [], steps[0]
    try:
        if recipe == "text-llm-node" and not (name and re.fullmatch(r"[A-Z][A-Za-z0-9]*", name)):
            raise RecipeError("A PascalCase --name is required for text-llm-node")
        pipeline_path = absolute(pipeline_path, root)
        conf_path = pipeline_path.with_suffix(".conf")
        require_deployment(conf_path)
        pipeline = read_json_file(pipeline_path)
        effects = absolute(effects_path, root)
        spec = read_json_file(effects)
        check_effects(spec)
        if spec.get("biz_name") != pipeline["biz_name"]:
            raise RecipeError("Effects biz_name does not match the task")
        if not absolute(spec["dataset"], effects.parent).is_file():
            raise RecipeError("Effects dataset is missing")
        if demo_path is None:
            raise RecipeError("--demo is required; use the prepared verification command")
        tool, build, demo = tool_context(tool_path, build_dir, root, require_tool=False, demo_path=demo_path)
        models = absolute(model_root, root)
        manifest = absolute(manifest_path, root) if manifest_path else root / "models/asset_manifest.json"
        VERIFY_SELECTION.validate_manifest(read_json_file(manifest))
        completed.append(step)
        if recipe == "text-llm-node" or not tool.is_file() or not demo.is_file():
            step = "build"
            if step not in steps:
                steps.insert(1, step)
            targets = [tool.name, "alg_demo"]
            if recipe == "text-llm-node":
                targets.append(SCAFFOLD.get_runner_info(root, build)[0])
            proc = subprocess.run(["cmake", "--build", str(build), "--target", *targets, "-j4"],
                                  cwd=root, capture_output=True, text=True, check=False)
            if proc.returncode:
                raise RecipeError("Build failed:\n" + (proc.stdout + proc.stderr)[-4000:])
            if not tool.is_file() or not demo.is_file():
                raise RecipeError("Build did not produce the selected tool and Demo")
            completed.append(step)
        if recipe == "text-llm-node":
            step = "catalog_discovery"
            catalog = native(tool, ["catalog"], root)
            if name not in {node["node_type"] for node in catalog["nodes"]}:
                raise RecipeError(f"Node {name} is absent from the selected executable Catalog")
            if not any(node["node_type"] == name for node in pipeline["pipeline"]):
                raise RecipeError(f"Pipeline does not use the requested Node {name}")
            completed.append(step)
        step = "config_validation"
        native(tool, ["validate", "--stdin"], root, pipeline)
        native(tool, ["plan", "--stdin"], root, pipeline)
        bundle = deployment_root(root, pipeline_path, models)
        resolved = native(tool, ["resolve-conf", str(conf_path.relative_to(bundle)), "--root", str(bundle)], root)
        configuration = resolved["configuration"]
        if Path(configuration["pipeline_path"]).resolve() != pipeline_path:
            raise RecipeError("Deployment points to a different Pipeline than --pipeline")
        # Compare the effective model paths with the asset selection used by evaluate.
        effective = configuration["effective_pipeline"]
        for model in effective.get("models", []):
            original = next(m for m in pipeline["models"] if m["model_id"] == model["model_id"])
            if absolute(model["model_path"], bundle) != VERIFY_SELECTION.within(models, original["model_path"]):
                raise RecipeError(f"Deployment model path differs from --model-root for {model['model_id']}")
        completed.append(step)
        if recipe == "text-llm-node":
            step = "focused_test"
            runner = build / SCAFFOLD.get_runner_info(root, build)[1]
            filter_arg = f"--gtest_filter=CustomNodeCatalogTest.{name}_*"
            discovery = subprocess.run([str(runner), filter_arg, "--gtest_list_tests"], cwd=root,
                                       capture_output=True, text=True, check=False)
            count = sum(bool(re.match(r"^  [A-Za-z_][A-Za-z0-9_]*(?:\s|$)", line)) for line in discovery.stdout.splitlines())
            if discovery.returncode or count == 0:
                raise RecipeError("Focused filter matched zero tests or discovery failed:\n" + discovery.stdout + discovery.stderr)
            proc = subprocess.run([str(runner), filter_arg], cwd=root, capture_output=True, text=True, check=False)
            passed = re.search(r"\[\s*PASSED\s*\]\s*(\d+)\s+test", proc.stdout)
            if proc.returncode or not passed or int(passed[1]) != count:
                raise RecipeError("Focused tests failed or skipped required cases:\n" + proc.stdout + proc.stderr)
            completed.append(step)
        step = "evaluate"
        selection = VERIFY_SELECTION.inspect_selection(pipeline, tool, models, manifest)
        if not selection["ok"]:
            raise RecipeError("Configuration, build or assets are not verified", selection)
        evaluated = VERIFY_SELECTION.evaluate(pipeline, selection, tool, models, effects, conf_path, demo)
        if evaluated.get("metrics", {}).get("status") != "passed":
            raise RecipeError("Effects evaluation failed", evaluated)
        completed.append(step)
        return result(recipe, completed, [], [pipeline_path, conf_path, effects], metrics=evaluated["metrics"])
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, StopIteration, subprocess.SubprocessError) as error:
        return result(recipe, completed, [s for s in steps if s not in completed], error=error, step=step)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    listing = subparsers.add_parser("list")
    listing.add_argument("--json", action="store_true")
    for operation in ("prepare", "verify"):
        sub = subparsers.add_parser(operation)
        sub.add_argument("recipe", choices=SUPPORTED_RECIPES)
        sub.add_argument("--name", required=operation == "prepare")
        sub.add_argument("--profile", required=operation == "prepare")
        sub.add_argument("--tool", type=Path, required=True)
        sub.add_argument("--build-dir", type=Path, required=True)
        sub.add_argument("--pipeline", type=Path, required=True)
        sub.add_argument("--effects", type=Path, required=operation == "verify")
        sub.add_argument("--model-root", type=Path, required=operation == "verify")
        sub.add_argument("--manifest", type=Path)
        sub.add_argument("--demo", type=Path)
        sub.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if args.command == "list":
        return list_recipes(args.json)
    if args.command == "prepare":
        report = prepare(args.recipe, args.name, args.profile, args.tool, args.build_dir, args.pipeline, ROOT,
                         args.effects, args.model_root, args.manifest)
    else:
        report = verify_recipe(args.recipe, args.pipeline, args.tool, args.build_dir, args.effects,
                               args.model_root, args.name, args.demo, args.manifest)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        print(f"{args.recipe}: {'PASSED' if report['ok'] else 'FAILED at ' + report['failed_step']}")
        if not report["ok"]:
            print(report["message"])
            if "report" in report:
                print(json.dumps(report["report"], ensure_ascii=False, indent=2))
        for artifact in report["artifacts"]:
            print(f"  {artifact}")
        for item in report["next_commands"]:
            print(f"  {item['description']}:\n    {item['command']}")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
